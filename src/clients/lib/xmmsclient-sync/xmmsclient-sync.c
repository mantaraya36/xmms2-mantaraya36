#include "xmmsclient.h"
#include "xmmsclient-glib.h"
#include "xmmsclient-sync.h"
#include "xmms/signal_xmms.h"
#include <glib.h>

static GMainLoop *mainloop;
static xmmsc_connection_t *xmmsc_conn;

static void
handle_witharg (void *userdata, void *arg)
{
	*((void **)userdata) = arg;
	g_main_loop_quit (mainloop);
}

static void *
xmmsc_sync_wait_cmd_arg (gchar *cmd)
{
	void *arg;

	/* setup callback to "cmd", run mainloop, 
	 * return arg passed to callback 
	 */


	xmmsc_set_callback (xmmsc_conn, cmd, handle_witharg, (void *) &arg);

	xmmsc_setup_with_gmain (xmmsc_conn, NULL);

	g_main_loop_run (mainloop);
	
	return arg;
}

void
xmmsc_sync_init (xmmsc_connection_t *conn)
{
	xmmsc_conn = conn;
	mainloop = g_main_loop_new (NULL, FALSE);
}

gint
xmmsc_sync_played_time_get ()
{
	void *pt;
	gint ret;
	
	/* not needed to send anything here, since
	 * PLAYTIME is send to all clients
	 */
	pt = xmmsc_sync_wait_cmd_arg (XMMS_SIGNAL_PLAYBACK_PLAYTIME);

	ret = GPOINTER_TO_UINT (pt);

	return ret;
}

