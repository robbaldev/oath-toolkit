/*
 * pam_hotp.c - a PAM module for HOTP one-time passwords
 * Copyright (C) 2009  Simon Josefsson
 *
 * This program is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see
 * <http://www.gnu.org/licenses/>.
 *
 */

#include <config.h>

#include "hotp.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <ctype.h>

/* Libtool defines PIC for shared objects */
#ifndef PIC
#define PAM_STATIC
#endif

/* These #defines must be present according to PAM documentation. */
#define PAM_SM_AUTH
#define PAM_SM_ACCOUNT
#define PAM_SM_SESSION
#define PAM_SM_PASSWORD

#ifdef HAVE_SECURITY_PAM_APPL_H
#include <security/pam_appl.h>
#endif
#ifdef HAVE_SECURITY_PAM_MODULES_H
#include <security/pam_modules.h>
#endif

#if defined(DEBUG_PAM)
# if defined(HAVE_SECURITY__PAM_MACROS_H)
#  define DEBUG
#  include <security/_pam_macros.h>
# else
#  define D(x) do {							\
    printf ("debug: %s:%d (%s): ", __FILE__, __LINE__, __FUNCTION__);	\
    printf x;								\
    printf ("\n");							\
  } while (0)
# endif
#endif

#include <ykclient.h>

#ifdef HAVE_LIBLDAP
#include <ldap.h>
#define PORT_NUMBER  LDAP_PORT
#endif

#ifndef PAM_EXTERN
#ifdef PAM_STATIC
#define PAM_EXTERN static
#else
#define PAM_EXTERN extern
#endif
#endif

#include <sys/types.h>
#include <pwd.h>

#define TOKEN_LEN 44
#define TOKEN_ID_LEN 12

struct cfg
{
  int debug;
  int alwaysok;
  int try_first_pass;
  int use_first_pass;
  char *auth_file;
};

static void
parse_cfg (int flags, int argc, const char **argv, struct cfg *cfg)
{
  int i;

  cfg->debug = 0;
  cfg->alwaysok = 0;
  cfg->try_first_pass = 0;
  cfg->use_first_pass = 0;
  cfg->auth_file = NULL;

  for (i = 0; i < argc; i++)
    {
      if (strcmp (argv[i], "debug") == 0)
	cfg->debug = 1;
      if (strcmp (argv[i], "alwaysok") == 0)
	cfg->alwaysok = 1;
      if (strcmp (argv[i], "try_first_pass") == 0)
	cfg->try_first_pass = 1;
      if (strcmp (argv[i], "use_first_pass") == 0)
	cfg->use_first_pass = 1;
      if (strncmp (argv[i], "authfile=", 9) == 0)
	cfg->auth_file = (char *) argv[i] + 9;
    }

  if (cfg->debug)
    {
      D (("called."));
      D (("flags %d argc %d", flags, argc));
      for (i = 0; i < argc; i++)
	D (("argv[%d]=%s", i, argv[i]));
      D (("debug=%d", cfg->debug));
      D (("alwaysok=%d", cfg->alwaysok));
      D (("try_first_pass=%d", cfg->try_first_pass));
      D (("use_first_pass=%d", cfg->use_first_pass));
      D (("authfile=%s", cfg->auth_file ? cfg->auth_file : "(null)"));
    }
}

#define DBG(x) if (cfg.debug) { D(x); }

PAM_EXTERN int
pam_sm_authenticate (pam_handle_t * pamh,
		     int flags, int argc, const char **argv)
{
  int retval, rc;
  const char *user = NULL;
  const char *password = NULL;
  char otp[TOKEN_LEN + 1] = { 0 };
  char otp_id[TOKEN_ID_LEN + 1] = { 0 };
  int password_len = 0;
  int valid_token = 0;
  struct pam_conv *conv;
  struct pam_message *pmsg[1], msg[1];
  struct pam_response *resp;
  int nargs = 1;
  struct cfg cfg;

  parse_cfg (flags, argc, argv, &cfg);

  retval = pam_get_user (pamh, &user, NULL);
  if (retval != PAM_SUCCESS)
    {
      DBG (("get user returned error: %s", pam_strerror (pamh, retval)));
      goto done;
    }
  DBG (("get user returned: %s", user));

  if (cfg.try_first_pass || cfg.use_first_pass)
    {
      retval = pam_get_item (pamh, PAM_AUTHTOK, (const void **) &password);
      if (retval != PAM_SUCCESS)
	{
	  DBG (("get password returned error: %s",
	      pam_strerror (pamh, retval)));
	  goto done;
	}
      DBG (("get password returned: %s", password));
    }

  if (cfg.use_first_pass && password == NULL)
    {
      DBG (("use_first_pass set and no password, giving up"));
      retval = PAM_AUTH_ERR;
      goto done;
    }

  rc = hotp_init ();
  if (rc != HOTP_OK)
    {
      DBG (("hotp_init() failed (%d)", rc));
      retval = PAM_AUTHINFO_UNAVAIL;
      goto done;
    }

  if (password == NULL)
    {
      retval = pam_get_item (pamh, PAM_CONV, (const void **) &conv);
      if (retval != PAM_SUCCESS)
	{
	  DBG (("get conv returned error: %s", pam_strerror (pamh, retval)));
	  goto done;
	}

      pmsg[0] = &msg[0];
      {
	const char *query_template = "One-time password (HOTP) for `%s': ";
	size_t len = strlen (query_template) + strlen (user);
	size_t wrote;

	msg[0].msg = malloc (len);
	if (!msg[0].msg)
	  {
	    retval = PAM_BUF_ERR;
	    goto done;
	  }

	wrote = snprintf ((char *) msg[0].msg, len, query_template, user);
	if (wrote < 0 || wrote >= len)
	  {
	    retval = PAM_BUF_ERR;
	    goto done;
	  }
      }
      msg[0].msg_style = PAM_PROMPT_ECHO_OFF;
      resp = NULL;

      retval = conv->conv (nargs, (const struct pam_message **) pmsg,
			   &resp, conv->appdata_ptr);

      free ((char *) msg[0].msg);

      if (retval != PAM_SUCCESS)
	{
	  DBG (("conv returned error: %s", pam_strerror (pamh, retval)));
	  goto done;
	}

      DBG (("conv returned: %s", resp->resp));

      password = resp->resp;
    }

  password_len = strlen (password);
  if (password_len < TOKEN_LEN)
    {
      DBG (("OTP too short: %s", password));
      retval = PAM_AUTH_ERR;
      goto done;
    }

  DBG (("OTP: %s", otp));

  /* user entered their system password followed by generated OTP? */
  if (password_len > TOKEN_LEN)
    {
      char *onlypasswd = strdup (password);

      onlypasswd[password_len - TOKEN_LEN] = '\0';

      DBG (("Password: %s ", onlypasswd));

      retval = pam_set_item (pamh, PAM_AUTHTOK, onlypasswd);
      free (onlypasswd);
      if (retval != PAM_SUCCESS)
	{
	  DBG (("set_item returned error: %s", pam_strerror (pamh, retval)));
	  goto done;
	}
    }
  else
    password = NULL;

  if (valid_token == 0)
    {
      DBG (("One-time password not authorized to login as user"));
      retval = PAM_AUTHINFO_UNAVAIL;
      goto done;
    }

  retval = PAM_SUCCESS;

done:
  hotp_done ();
  if (cfg.alwaysok && retval != PAM_SUCCESS)
    {
      DBG (("alwaysok needed (otherwise return with %d)", retval));
      retval = PAM_SUCCESS;
    }
  DBG (("done. [%s]", pam_strerror (pamh, retval)));
  pam_set_data (pamh, "hotp_setcred_return", (void*) (intptr_t) retval, NULL);

  return retval;
}

PAM_EXTERN int
pam_sm_setcred (pam_handle_t * pamh, int flags, int argc, const char **argv)
{
  int retval;
  int auth_retval;
  struct cfg cfg;

  parse_cfg (flags, argc, argv, &cfg);

  DBG (("called."));

  /* TODO: ? */

  retval = pam_get_data (pamh, "hotp_setcred_return",
			 (void*) (intptr_t) &auth_retval);
  DBG (("retval: %d", auth_retval));
  if (retval != PAM_SUCCESS)
    return PAM_CRED_UNAVAIL;

  switch (auth_retval)
    {
    case PAM_SUCCESS:
      retval = PAM_SUCCESS;
      break;

    case PAM_USER_UNKNOWN:
      retval = PAM_USER_UNKNOWN;
      break;

    case PAM_AUTH_ERR:
    default:
      retval = PAM_CRED_ERR;
      break;
    }

  DBG (("done. [%s]", pam_strerror (pamh, retval)));

  return retval;
}

PAM_EXTERN int
pam_sm_acct_mgmt (pam_handle_t * pamh, int flags, int argc, const char **argv)
{
  int retval;

  D (("called."));

  /* TODO: ? */
  retval = PAM_SUCCESS;

  D (("done. [%s]", pam_strerror (pamh, retval)));

  return retval;
}

PAM_EXTERN int
pam_sm_open_session (pam_handle_t * pamh,
		     int flags, int argc, const char **argv)
{
  int retval;

  D (("called."));

  /* TODO: ? */
  retval = PAM_SUCCESS;

  D (("done. [%s]", pam_strerror (pamh, retval)));

  return retval;
}

PAM_EXTERN int
pam_sm_close_session (pam_handle_t * pamh,
		      int flags, int argc, const char **argv)
{
  int retval;

  D (("called."));

  /* TODO: ? */
  retval = PAM_SUCCESS;

  D (("done. [%s]", pam_strerror (pamh, retval)));

  return retval;
}

PAM_EXTERN int
pam_sm_chauthtok (pam_handle_t * pamh, int flags, int argc, const char **argv)
{
  int retval;

  D (("called."));

  /* TODO: ? */
  retval = PAM_SUCCESS;

  D (("done. [%s]", pam_strerror (pamh, retval)));

  return retval;
}

#ifdef PAM_STATIC

struct pam_module _pam_hotp_modstruct = {
  "pam_hotp",
  pam_sm_authenticate,
  pam_sm_setcred,
  pam_sm_acct_mgmt,
  pam_sm_open_session,
  pam_sm_close_session,
  pam_sm_chauthtok
};

#endif