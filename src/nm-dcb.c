/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* NetworkManager -- Network link manager
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Copyright (C) 2013 Red Hat, Inc.
 */

#include <config.h>
#include <sys/wait.h>
#include <string.h>

#include <glib.h>
#include "nm-dcb.h"
#include "nm-platform.h"
#include "NetworkManagerUtils.h"
#include "nm-posix-signals.h"
#include "nm-logging.h"

GQuark
nm_dcb_error_quark (void)
{
	static GQuark ret = 0;

	if (ret == 0)
		ret = g_quark_from_static_string ("nm-dcb-error");
	return ret;
}

static const char *helper_names[] = { "dcbtool", "fcoeadm" };

gboolean
do_helper (const char *iface,
           guint which,
           DcbFunc run_func,
           gpointer user_data,
           GError **error,
           const char *fmt,
           ...)
{
	char **argv = NULL, **split = NULL, *cmdline, *errmsg = NULL;
	gboolean success = FALSE;
	guint i, u;
	va_list args;

	g_return_val_if_fail (fmt != NULL, FALSE);

	va_start (args, fmt);
	cmdline = g_strdup_vprintf (fmt, args);
	va_end (args);

	split = g_strsplit_set (cmdline, " ", 0);
	if (!split) {
		g_set_error (error, NM_DCB_ERROR, NM_DCB_ERROR_INTERNAL,
		             "failure parsing %s command line", helper_names[which]);
		goto out;
	}

	/* Allocate space for path, custom arg, interface name, arguments, and NULL */
	i = u = 0;
	argv = g_new0 (char *, g_strv_length (split) + 4);
	argv[i++] = NULL;  /* Placeholder for dcbtool path */
	if (which == DCBTOOL) {
		argv[i++] = "sc";
		argv[i++] = (char *) iface;
	}
	while (u < g_strv_length (split))
		argv[i++] = split[u++];
	argv[i++] = NULL;
	success = run_func (argv, which, user_data, error);
	if (!success && error)
		g_assert (*error);

out:
	if (split)
		g_strfreev (split);
	g_free (argv);
	g_free (cmdline);
	g_free (errmsg);
	return success;
}

#define SET_FLAGS(f, tag) \
G_STMT_START { \
	if (!do_helper (iface, DCBTOOL, run_func, user_data, error, tag " e:%c a:%c w:%c", \
	                 f & NM_SETTING_DCB_FLAG_ENABLE ? '1' : '0', \
	                 f & NM_SETTING_DCB_FLAG_ADVERTISE ? '1' : '0', \
	                 f & NM_SETTING_DCB_FLAG_WILLING ? '1' : '0')) \
		return FALSE; \
} G_STMT_END

#define SET_APP(f, s, tag) \
G_STMT_START { \
	gint prio = nm_setting_dcb_get_app_##tag##_priority (s); \
 \
	SET_FLAGS (f, "app:" #tag); \
	if ((f & NM_SETTING_DCB_FLAG_ENABLE) && (prio >= 0)) { \
		if (!do_helper (iface, DCBTOOL, run_func, user_data, error, "app:" #tag " appcfg:%02x", (1 << prio))) \
			return FALSE; \
	} \
} G_STMT_END

gboolean
_dcb_setup (const char *iface,
            NMSettingDcb *s_dcb,
            DcbFunc run_func,
            gpointer user_data,
            GError **error)
{
	NMSettingDcbFlags flags;
	guint i;

	g_assert (s_dcb);

	if (!do_helper (iface, DCBTOOL, run_func, user_data, error, "dcb on"))
		return FALSE;

	/* FCoE */
	flags = nm_setting_dcb_get_app_fcoe_flags (s_dcb);
	SET_APP (flags, s_dcb, fcoe);

	/* iSCSI */
	flags = nm_setting_dcb_get_app_iscsi_flags (s_dcb);
	SET_APP (flags, s_dcb, iscsi);

	/* FIP */
	flags = nm_setting_dcb_get_app_fip_flags (s_dcb);
	SET_APP (flags, s_dcb, fip);

	/* Priority Flow Control */
	flags = nm_setting_dcb_get_priority_flow_control_flags (s_dcb);
	SET_FLAGS (flags, "pfc");
	if (flags & NM_SETTING_DCB_FLAG_ENABLE) {
		char buf[10];

		for (i = 0; i < 8; i++)
			buf[i] = nm_setting_dcb_get_priority_flow_control (s_dcb, i) ? '1' : '0';
		buf[i] = 0;
		if (!do_helper (iface, DCBTOOL, run_func, user_data, error, "pfc pfcup:%s", buf))
			return FALSE;
	}

	/* Priority Groups */
	flags = nm_setting_dcb_get_priority_group_flags (s_dcb);
	if (flags & NM_SETTING_DCB_FLAG_ENABLE) {
		GString *s;
		gboolean success;
		guint id;

		s = g_string_sized_new (150);

		g_string_append_printf (s, "pg e:1 a:%c w:%c",
		                        flags & NM_SETTING_DCB_FLAG_ADVERTISE ? '1' : '0',
		                        flags & NM_SETTING_DCB_FLAG_WILLING ? '1' : '0');

		/* Priority Groups */
		g_string_append (s, " pgid:");
		for (i = 0; i < 8; i++) {
			id = nm_setting_dcb_get_priority_group_id (s_dcb, i);
			g_assert (id < 8 || id == 15);
			g_string_append_c (s, (id < 8) ? ('0' + id) : 'f');
		}

		/* Priority Group Bandwidth */
		g_string_append_printf (s, " pgpct:%u,%u,%u,%u,%u,%u,%u,%u",
		                        nm_setting_dcb_get_priority_group_bandwidth (s_dcb, 0),
		                        nm_setting_dcb_get_priority_group_bandwidth (s_dcb, 1),
		                        nm_setting_dcb_get_priority_group_bandwidth (s_dcb, 2),
		                        nm_setting_dcb_get_priority_group_bandwidth (s_dcb, 3),
		                        nm_setting_dcb_get_priority_group_bandwidth (s_dcb, 4),
		                        nm_setting_dcb_get_priority_group_bandwidth (s_dcb, 5),
		                        nm_setting_dcb_get_priority_group_bandwidth (s_dcb, 6),
		                        nm_setting_dcb_get_priority_group_bandwidth (s_dcb, 7));

		/* Priority Bandwidth */
		g_string_append_printf (s, " uppct:%u,%u,%u,%u,%u,%u,%u,%u",
		                        nm_setting_dcb_get_priority_bandwidth (s_dcb, 0),
		                        nm_setting_dcb_get_priority_bandwidth (s_dcb, 1),
		                        nm_setting_dcb_get_priority_bandwidth (s_dcb, 2),
		                        nm_setting_dcb_get_priority_bandwidth (s_dcb, 3),
		                        nm_setting_dcb_get_priority_bandwidth (s_dcb, 4),
		                        nm_setting_dcb_get_priority_bandwidth (s_dcb, 5),
		                        nm_setting_dcb_get_priority_bandwidth (s_dcb, 6),
		                        nm_setting_dcb_get_priority_bandwidth (s_dcb, 7));

		/* Strict Bandwidth */
		g_string_append (s, " strict:");
		for (i = 0; i < 8; i++)
			g_string_append_c (s, nm_setting_dcb_get_priority_strict_bandwidth (s_dcb, i) ? '1' : '0');

		/* Priority Traffic Class */
		g_string_append (s, " up2tc:");
		for (i = 0; i < 8; i++) {
			id = nm_setting_dcb_get_priority_traffic_class (s_dcb, i);
			g_assert (id < 8);
			g_string_append_c (s, '0' + id);
		}

		success = do_helper (iface, DCBTOOL, run_func, user_data, error, s->str);
		g_string_free (s, TRUE);
		if (!success)
			return FALSE;
	} else {
		/* Ignore disable failure since lldpad <= 0.9.46 does not support disabling
		 * priority groups without specifying an entire PG config.
		 */
		do_helper (iface, DCBTOOL, run_func, user_data, error, "pg e:0");
	}

	return TRUE;
}

gboolean
_dcb_cleanup (const char *iface,
              DcbFunc run_func,
              gpointer user_data,
              GError **error)
{
	/* FIXME: do we need to turn off features individually here? */
	return do_helper (iface, DCBTOOL, run_func, user_data, error, "dcb off");
}

gboolean
_fcoe_setup (const char *iface,
             NMSettingDcb *s_dcb,
             DcbFunc run_func,
             gpointer user_data,
             GError **error)
{
	NMSettingDcbFlags flags;

	g_assert (s_dcb);

	flags = nm_setting_dcb_get_app_fcoe_flags (s_dcb);
	if (flags & NM_SETTING_DCB_FLAG_ENABLE) {
		const char *mode = nm_setting_dcb_get_app_fcoe_mode (s_dcb);

		if (!do_helper (NULL, FCOEADM, run_func, user_data, error, "-m %s -c %s", mode, iface))
			return FALSE;
	} else {
		if (!do_helper (NULL, FCOEADM, run_func, user_data, error, "-d %s", iface))
			return FALSE;
	}

	return TRUE;
}

gboolean
_fcoe_cleanup (const char *iface,
               DcbFunc run_func,
               gpointer user_data,
               GError **error)
{
	return do_helper (NULL, FCOEADM, run_func, user_data, error, "-d %s", iface);
}


static const char *dcbpaths[] = {
	"/sbin/dcbtool",
	"/usr/sbin/dcbtool",
	"/usr/local/sbin/dcbtool",
	NULL
};
static const char *fcoepaths[] = {
	"/sbin/fcoeadm",
	"/usr/sbin/fcoeadm",
	"/usr/local/sbin/fcoeadm",
	NULL
};


static gboolean
run_helper (char **argv, guint which, gpointer user_data, GError **error)
{
	static const char *helper_path[2] = { NULL, NULL };
	int exit_status = 0;
	gboolean success;
	char *errmsg = NULL, *outmsg = NULL;
	const char **iter;
	char *cmdline;

	if (G_UNLIKELY (helper_path[which] == NULL)) {
		iter = (which == DCBTOOL) ? dcbpaths : fcoepaths;
		while (*iter) {
			if (g_file_test (*iter, G_FILE_TEST_EXISTS))
				helper_path[which] = *iter;
			iter++;
		}
		if (!helper_path[which]) {
			g_set_error (error, NM_DCB_ERROR, NM_DCB_ERROR_HELPER_NOT_FOUND,
			             "%s not found",
			             which == DCBTOOL ? "dcbtool" : "fcoadm");
			return FALSE;
		}
	}

	argv[0] = (char *) helper_path[which];
	cmdline = g_strjoinv (" ", argv);
	nm_log_dbg (LOGD_DCB, "%s", cmdline);

	success = g_spawn_sync ("/", argv, NULL, 0 /*G_SPAWN_DEFAULT*/,
	                        nm_unblock_posix_signals, NULL,
	                        &outmsg, &errmsg, &exit_status, error);
	/* Log any stderr output */
	if (success && WIFEXITED (exit_status) && WEXITSTATUS (exit_status) && (errmsg || outmsg)) {
		nm_log_dbg (LOGD_DCB, "'%s' failed: '%s'",
		            cmdline, (errmsg && strlen (errmsg)) ? errmsg : outmsg);
		g_set_error (error, NM_DCB_ERROR, NM_DCB_ERROR_HELPER_FAILED,
		             "Failed to run '%s'", cmdline);
		success = FALSE;
	}
	g_free (outmsg);
	g_free (errmsg);

	g_free (cmdline);
	return success;
}

gboolean
nm_dcb_setup (const char *iface, NMSettingDcb *s_dcb, GError **error)
{
	gboolean success;

	success = _dcb_setup (iface, s_dcb, run_helper, GUINT_TO_POINTER (DCBTOOL), error);
	if (success)
		success = _fcoe_setup (iface, s_dcb, run_helper, GUINT_TO_POINTER (FCOEADM), error);

	return success;
}

gboolean
nm_dcb_cleanup (const char *iface, GError **error)
{
	gboolean success;

	success = _dcb_cleanup (iface, run_helper, GUINT_TO_POINTER (DCBTOOL), error);
	if (success) {
		/* Only report FCoE errors if DCB cleanup was successful */
		success = _fcoe_cleanup (iface, run_helper, GUINT_TO_POINTER (FCOEADM), success ? error : NULL);
	}

	return success;
}

