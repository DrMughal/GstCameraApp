/* GStreamer
 * Copyright (C) 2008 Wim Taymans <wim.taymans at gmail.com>
 * Copyright (C) 2014 Jan Schmidt <jan@centricular.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <stdlib.h>

#include <gst/gst.h>
#include <gst/net/gstnet.h>

#define PLAYBACK_DELAY_MS 200




/* Structure to contain all our information, so we can pass it to callbacks */
typedef struct _CustomData {
    GstElement *pipe, *src, *videoconvert, *filter, *videosink;
    GstElement *aud_conv, *audio_sink;
    GstClock *net_clock;
    GMainLoop *loop;  /* GLib's Main Loop */
} CustomData;


/* Handler for the pad-added signal */
static void pad_added_handler (GstElement *src, GstPad *pad, CustomData *data);


static void
source_created (GstElement * pipe, GstElement * source)
{
    g_object_set (source, "latency", PLAYBACK_DELAY_MS,
                  "ntp-time-source", 3, "buffer-mode", 4, "ntp-sync", TRUE, "rtcp-sync-send-time", FALSE,  NULL);
}


static gboolean
message (GstBus * bus, GstMessage * message, gpointer user_data)
{
  GMainLoop *loop = user_data;

  switch (GST_MESSAGE_TYPE (message)) {
    case GST_MESSAGE_ERROR:{
      GError *err = NULL;
      gchar *name, *debug = NULL;

      name = gst_object_get_path_string (message->src);
      gst_message_parse_error (message, &err, &debug);

      g_printerr ("ERROR: from element %s: %s\n", name, err->message);
      if (debug != NULL)
        g_printerr ("Additional debug info:\n%s\n", debug);

      g_error_free (err);
      g_free (debug);
      g_free (name);

      g_main_loop_quit (loop);
      break;
    }
    case GST_MESSAGE_WARNING:{
      GError *err = NULL;
      gchar *name, *debug = NULL;

      name = gst_object_get_path_string (message->src);
      gst_message_parse_warning (message, &err, &debug);

      g_printerr ("ERROR: from element %s: %s\n", name, err->message);
      if (debug != NULL)
        g_printerr ("Additional debug info:\n%s\n", debug);

      g_error_free (err);
      g_free (debug);
      g_free (name);
      break;
    }
    case GST_MESSAGE_EOS:
      g_print ("Got EOS\n");
      g_main_loop_quit (loop);
      break;
    default:
      break;
  }

  return TRUE;
}





int
main (int argc, char *argv[])
{

  gchar *server;
  gint clock_port;
  CustomData data;



  gst_init (&argc, &argv);

  /*if (argc < 2) {
    g_print ("usage: %s rtsp://URI clock-IP clock-PORT\n"
        "example: %s rtsp://localhost:8554/test 127.0.0.1 8554\n",
        argv[0], argv[0]);
    return -1;
  }*/

  //server = argv[2];
  //clock_port = atoi (argv[3]);

  //net_clock = gst_net_client_clock_new ("net_clock", server, clock_port, 0);
  server="se.pool.ntp.org";
    clock_port= 123;
    data.net_clock = gst_ntp_clock_new ("net_clock", server, clock_port, 0);
  if (data.net_clock == NULL) {
    g_print ("Failed to create net clock client for %s:%d\n",
        server, clock_port);
    return 1;
  }

  /* Wait for the clock to stabilise */
  gst_clock_wait_for_sync (data.net_clock, GST_CLOCK_TIME_NONE);

  data.loop = g_main_loop_new (NULL, FALSE);


    /* Create the elements */

    data.src=gst_element_factory_make ("uridecodebin", "src");
    data.videoconvert = gst_element_factory_make ("videoconvert", "video_convert");
    data.filter = gst_element_factory_make("capsfilter", "filter");
    data.videosink = gst_element_factory_make ("osxvideosink", "video_sink");

    data.aud_conv = gst_element_factory_make ("audioconvert", "aud_conv");
    data.audio_sink = gst_element_factory_make ("autoaudiosink", "audio_sink");

    GstCaps *new_caps;
    /*new_caps = gst_caps_new_simple ("video/x-raw",
        "width", G_TYPE_INT, 480,
        "height", G_TYPE_INT, 270,
        "format", G_TYPE_STRING, "ARGB"
        , NULL);
*/
new_caps= gst_caps_from_string("video/x-raw, format=ARGB");
    g_object_set (data.filter,
        "caps", new_caps,
        NULL);


    /* Create the empty pipeline */
    data.pipe = gst_pipeline_new ("test-pipeline");

    if (!data.pipe || !data.src || !data.videoconvert || !data.videosink || !data.aud_conv || !data.audio_sink ) {
        g_printerr ("Not all elements could be created.\n");
        return -1;
    }


   /* Add and Link all elements that can be automatically linked because they have "Always" pads */
    gst_bin_add_many (GST_BIN (data.pipe), data.src, data.videoconvert, data.videosink, data.aud_conv, data.audio_sink, NULL);

    if (gst_element_link_many ( data.videoconvert,  data.videosink, NULL) != TRUE ||
        gst_element_link_many ( data.aud_conv,  data.audio_sink, NULL) != TRUE)
    {
        g_printerr ("Elements could not be linked.\n");
        gst_object_unref (data.pipe);
        return -1;
    }






  g_object_set (data.src, "uri", argv[1], NULL);


    /* connect uridecode bin signal*/
  g_signal_connect (data.src, "source-setup", G_CALLBACK (source_created), NULL);
   /* connect pad-added signal from uridecodebin*/
  g_signal_connect (data.src, "pad-added", G_CALLBACK (pad_added_handler), &data);


  gst_pipeline_use_clock (GST_PIPELINE (data.pipe), data.net_clock);

  /* Set this high enough so that it's higher than the minimum latency
   * on all receivers */
  gst_pipeline_set_latency (GST_PIPELINE (data.pipe), 1500 * GST_MSECOND);

  if (gst_element_set_state (data.pipe,
          GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    g_print ("Failed to set state to PLAYING\n");
    goto exit;
  };

  gst_bus_add_signal_watch (GST_ELEMENT_BUS (data.pipe));
  g_signal_connect (GST_ELEMENT_BUS (data.pipe), "message", G_CALLBACK (message),
      data.loop);

  g_main_loop_run (data.loop);

exit:
  gst_element_set_state (data.pipe, GST_STATE_NULL);
  gst_object_unref (data.pipe);
  g_main_loop_unref (data.loop);

  return 0;
}




static void pad_added_handler (GstElement *src, GstPad *new_pad, CustomData *data) {
    GstPad *audio_sink_pad = gst_element_get_static_pad (data->aud_conv, "sink");
    GstPad *video_sink_pad = gst_element_get_static_pad (data->videoconvert, "sink");

    GstPadLinkReturn ret;
    GstCaps *new_pad_caps = NULL;
    GstStructure *new_pad_struct = NULL;
    const gchar *new_pad_type = NULL;
    g_print ("\n Received new pad '%s' from '%s':\n", GST_PAD_NAME (new_pad), GST_ELEMENT_NAME (src));

    /* If our converter is already linked, we have nothing to do here */
    if (gst_pad_is_linked (audio_sink_pad) && gst_pad_is_linked (video_sink_pad)) {
        g_print ("  We are already linked. Ignoring.\n");
        goto exit;
    }

    /* Check the new pad's type */
    new_pad_caps = gst_pad_query_caps (new_pad, NULL);
    new_pad_struct = gst_caps_get_structure (new_pad_caps, 0);
    new_pad_type = gst_structure_get_name (new_pad_struct);
    g_print ("  New pad type is:  '%s' ", new_pad_type);

    if (g_str_has_prefix (new_pad_type, "audio/x-raw") ) {
        /* Attempt the link */
        ret = gst_pad_link (new_pad, audio_sink_pad);
        if (GST_PAD_LINK_FAILED (ret)) {
            g_print ("  Type is '%s' but link failed.\n", new_pad_type);
        } else {
            g_print ("  Link succeeded (type '%s').\n", new_pad_type);
        }

    } else if(g_str_has_prefix (new_pad_type, "video/x-raw"))
    {
        g_print ("  Attempting to link video pad\n");

        /* Attempt the  link */
        ret = gst_pad_link (new_pad, video_sink_pad);
        if (GST_PAD_LINK_FAILED (ret)) {
            g_print ("  Type is '%s' but link failed.\n", new_pad_type);
        } else {
            g_print ("  Link succeeded (type '%s').\n", new_pad_type);
        }


    }else goto exit;


exit:
    /* Unreference the new pad's caps, if we got them */
    if (new_pad_caps != NULL)
        gst_caps_unref (new_pad_caps);

    /* Unreference the sink pad */
    // gst_object_unref (audio_sink_pad);
    gst_object_unref (video_sink_pad);
}
