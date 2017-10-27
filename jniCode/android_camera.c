/* Manual Pipeline Intervidio Parse Launch pipeline
 * Copyright (C) 2012, Collabora Ltd.
 *   Author: Youness Alaoui
 *
 * Copyright (C) 2016-2017, Collabora Ltd.
 *   Author: Justin Kim <justin.kim@collabora.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation
 * version 2.1 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 *
 */

#include <stdlib.h>
#include <string.h>
#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <gst/gst.h>
#include <pthread.h>
#include <gst/video/videooverlay.h>
#include <gst/interfaces/photography.h>

#include <gst/net/gstnettimeprovider.h>
#include <gst/rtsp-server/rtsp-server.h>
#include <gst/video/video.h>

GST_DEBUG_CATEGORY_STATIC (debug_category);
#define GST_CAT_DEFAULT debug_category

/*
 * These macros provide a way to store the native pointer to GstAhc, which might be 32 or 64 bits, into
 * a jlong, which is always 64 bits, without warnings.
 */
#if GLIB_SIZEOF_VOID_P == 8
# define GET_CUSTOM_DATA(env, thiz, fieldID) (GstAhc *)(*env)->GetLongField (env, thiz, fieldID)
# define SET_CUSTOM_DATA(env, thiz, fieldID, data) (*env)->SetLongField (env, thiz, fieldID, (jlong)data)
#else
# define GET_CUSTOM_DATA(env, thiz, fieldID) (GstAhc *)(jint)(*env)->GetLongField (env, thiz, fieldID)
# define SET_CUSTOM_DATA(env, thiz, fieldID, data) (*env)->SetLongField (env, thiz, fieldID, (jlong)(jint)data)
#endif




#define TEST_TYPE_RTSP_MEDIA_FACTORY      (test_rtsp_media_factory_get_type ())
#define TEST_TYPE_RTSP_MEDIA              (test_rtsp_media_get_type ())

/* for RTSP test-netclock.c */
GstClock *global_clock;


GType test_rtsp_media_get_type (void);

typedef struct TestRTSPMediaClass TestRTSPMediaClass;
typedef struct TestRTSPMedia TestRTSPMedia;

struct TestRTSPMediaClass
{
    GstRTSPMediaClass parent;
};

struct TestRTSPMedia
{
    GstRTSPMedia parent;
};

static gboolean custom_setup_rtpbin (GstRTSPMedia * media, GstElement * rtpbin);

G_DEFINE_TYPE (TestRTSPMedia, test_rtsp_media, GST_TYPE_RTSP_MEDIA);

static void
test_rtsp_media_class_init (TestRTSPMediaClass * test_klass)
{
    GstRTSPMediaClass *klass = (GstRTSPMediaClass *) (test_klass);
    klass->setup_rtpbin = custom_setup_rtpbin;
}

static void
test_rtsp_media_init (TestRTSPMedia * media)
{
}

static gboolean
custom_setup_rtpbin (GstRTSPMedia * media, GstElement * rtpbin)
{
    g_object_set (rtpbin, "ntp-time-source", 3, NULL);
    return TRUE;
}


/*Above is from RTSP -- test-netclock.c*/



typedef struct _GstAhc
{
  jobject app;
  GstElement *pipeline;
  GMainLoop *main_loop;
  ANativeWindow *native_window;
  gboolean state;
  GstElement *ahcsrc;

  GstElement *vfilter1, *vfilter2;// *filter3;
  GstElement *vsink;
  gboolean initialized;
     /*For RTSP SERVER*/

    GstRTSPServer *server;
    GstRTSPMountPoints *mounts;
    GstRTSPMediaFactory *factory;

} GstAhc;

static pthread_t gst_app_thread;
static pthread_key_t current_jni_env;
static JavaVM *java_vm;
static jfieldID native_android_camera_field_id;
static jmethodID on_error_method_id;
static jmethodID on_state_changed_method_id;
static jmethodID on_gstreamer_initialized_method_id;



/*
 * Private methods
 */
static JNIEnv *
attach_current_thread (void)
{
  JNIEnv *env;
  JavaVMAttachArgs args;

  GST_DEBUG ("Attaching thread %p", g_thread_self ());
  args.version = JNI_VERSION_1_4;
  args.name = NULL;
  args.group = NULL;

  if ((*java_vm)->AttachCurrentThread (java_vm, &env, &args) < 0) {
    GST_ERROR ("Failed to attach current thread");
    return NULL;
  }

  return env;
}

static void
detach_current_thread (void *env)
{
  GST_DEBUG ("Detaching thread %p", g_thread_self ());
  (*java_vm)->DetachCurrentThread (java_vm);
}

static JNIEnv *
get_jni_env (void)
{
  JNIEnv *env;

  if ((env = pthread_getspecific (current_jni_env)) == NULL) {
    env = attach_current_thread ();
    pthread_setspecific (current_jni_env, env);
  }

  return env;
}

static void
on_error (GstBus * bus, GstMessage * message, GstAhc * ahc)
{
  gchar *message_string;
  GError *err;
  gchar *debug_info;
  jstring jmessage;
  JNIEnv *env = get_jni_env ();

  gst_message_parse_error (message, &err, &debug_info);
  message_string =
      g_strdup_printf ("Error received from element %s: %s",
      GST_OBJECT_NAME (message->src), err->message);

  g_clear_error (&err);
  g_free (debug_info);

  jmessage = (*env)->NewStringUTF (env, message_string);

  (*env)->CallVoidMethod (env, ahc->app, on_error_method_id, jmessage);
  if ((*env)->ExceptionCheck (env)) {
    GST_ERROR ("Failed to call Java method");
    (*env)->ExceptionClear (env);
  }
  (*env)->DeleteLocalRef (env, jmessage);

  g_free (message_string);
  gst_element_set_state (ahc->pipeline, GST_STATE_NULL);
}


static void
eos_cb (GstBus * bus, GstMessage * msg, GstAhc * data)
{
  gst_element_set_state (data->pipeline, GST_STATE_PAUSED);
}

static void
state_changed_cb (GstBus * bus, GstMessage * msg, GstAhc * ahc)
{
  JNIEnv *env = get_jni_env ();
  GstState old_state, new_state, pending_state;

  gst_message_parse_state_changed (msg, &old_state, &new_state, &pending_state);
  /* Only pay attention to messages coming from the pipeline, not its children */
  if (GST_MESSAGE_SRC (msg) == GST_OBJECT (ahc->pipeline)) {
    ahc->state = new_state;
    GST_DEBUG ("State changed to %s, notifying application",
        gst_element_state_get_name (new_state));
    (*env)->CallVoidMethod (env, ahc->app, on_state_changed_method_id,
        new_state);
    if ((*env)->ExceptionCheck (env)) {
      (*env)->ExceptionDescribe (env);
      GST_ERROR ("Failed to call Java method");
      (*env)->ExceptionClear (env);
    }
  }
}

static void
check_initialization_complete (GstAhc * data)
{
  JNIEnv *env = get_jni_env ();
  /* Check if all conditions are met to report GStreamer as initialized.
   * These conditions will change depending on the application */
  if (!data->initialized && data->native_window && data->main_loop) {
    GST_DEBUG
        ("Initialization complete, notifying application. native_window:%p main_loop:%p",
        data->native_window, data->main_loop);
    data->initialized = TRUE;
    (*env)->CallVoidMethod (env, data->app, on_gstreamer_initialized_method_id);
    if ((*env)->ExceptionCheck (env)) {
      GST_ERROR ("Failed to call Java method");
      (*env)->ExceptionClear (env);
    }
  }
}





/* called when a new media pipeline is prepared. */
static void
media_prepared (GstRTSPMedia *media, GstAhc *     user_data)

{
    GstElement *rtsp_pipeline;


    /* get the element used for providing the streams of the media */
    rtsp_pipeline = gst_rtsp_media_get_element (media);

   // gst_element_set_base_time(rtsp_pipeline, gst_element_get_base_time (user_data->pipeline));

    gst_element_set_base_time(user_data->pipeline, gst_element_get_base_time (rtsp_pipeline));

    // gst_element_set_base_time(rtsp_pipeline, gst_element_get_base_time (user_data->pipeline));
    gst_element_set_start_time (user_data->pipeline, GST_CLOCK_TIME_NONE);
    gst_element_set_start_time (rtsp_pipeline, GST_CLOCK_TIME_NONE);



    //gst_pipeline_set_latency (GST_PIPELINE (rtsp_pipeline), 200 * 1000000);

    g_print( "Pipleline Base Time : %llu \n",gst_element_get_base_time (user_data->pipeline)/1000000);

    g_print( "Rtsp_Pipe Base Time : %llu \n",gst_element_get_base_time (rtsp_pipeline)/1000000);
    g_print( "Rtsp_media Base Time : %llu \n",gst_rtsp_media_get_base_time (media)/1000000);

    g_print( "Current time on Pipeline : %llu \n", gst_clock_get_time(gst_element_get_clock (user_data->pipeline))/1000000);

    g_print( "Current time on Rtsp Pipe : %llu \n", gst_clock_get_time(gst_element_get_clock (rtsp_pipeline))/1000000);

    g_print( "Current time on Rtsp Media : %llu \n", gst_clock_get_time(gst_rtsp_media_get_clock (media))/1000000);

    g_print( "Current time on global_clock : %llu \n", gst_clock_get_time(global_clock)/1000000);




}

/* called when a new media pipeline is constructed. We can query the
 * pipeline and configure our appsrc */
static void
media_configure (GstRTSPMediaFactory * factory, GstRTSPMedia * media,
                 GstAhc * user_data)
{
    GstElement *rtsp_pipeline;
    rtsp_pipeline = gst_rtsp_media_get_element (media);



    user_data->vfilter2 = gst_bin_get_by_name_recurse_up (GST_BIN (rtsp_pipeline), "filter2");



   // g_signal_connect (media, "prepared", (GCallback) media_prepared,
    //       user_data);

}

static void *
app_function (void *userdata)
{
  JavaVMAttachArgs args;
  GstBus *bus;
  GstMessage *msg;
  GstAhc *ahc = (GstAhc *) userdata;
  GSource *bus_source;
  GMainContext *context;


  GST_DEBUG ("Creating pipeline in GstAhc at %p", ahc);
  GError *err = NULL;
  /* create our own GLib Main Context, so we do not interfere with other libraries using GLib */
  context = g_main_context_new ();


  ahc->vsink = gst_element_factory_make ("glimagesink", "vsink");
//  ahc->filter = gst_element_factory_make ("capsfilter", NULL);


   global_clock = gst_ntp_clock_new ("pool", "se.pool.ntp.org", 123, 0);

    /* Wait for the clock to stabilise */
   // gst_clock_wait_for_sync (global_clock, GST_CLOCK_TIME_NONE);

    /* create a server instance */
   ahc->server = gst_rtsp_server_new ();
    /* get the mount points for this server, every server has a default object
   * that be used to map uri mount points to media factories */
   ahc->mounts= gst_rtsp_server_get_mount_points (ahc->server);
    /* make a media factory for a test stream. The default media factory can use
   * gst-launch syntax to create pipelines.
   * any launch line works as long as it contains elements named pay%d. Each
   * element with pay%d names will be a stream */

    ahc->factory= gst_rtsp_media_factory_new ();

    /*this profile ensures quicker re-syncing but streams cant be played by vlc of ffplay. */// I added this to fix audio/video lag in viewfinder version. but it did not work
    //gst_rtsp_media_factory_set_profiles (ahc->factory, GST_RTSP_PROFILE_AVPF);


    /* NULL will force pipeline to run as fast as possible without any clock*/
    gst_pipeline_use_clock((GstPipeline*)ahc->pipeline,global_clock);
    //gst_element_set_start_time (ahc->pipeline, GST_CLOCK_TIME_NONE);






    ahc->pipeline= gst_parse_launch( " ahcsrc name=camera !  videoscale ! videoconvert ! video/x-raw, framerate=30/1 ! capsfilter name=filter1 caps=video/x-raw,width=960,height=540 ! tee name=t ! queue  ! glimagesink name=vidsink t. ! intervideosink  channel=liveling  sync=false  ", &err );


    if (err) {
        g_print("Unable to build pipeline: %s", err->message);
        g_clear_error (&err);

        return 0;
    }
    else {

        ahc->vsink=gst_bin_get_by_name(GST_BIN(ahc->pipeline), "vidsink");
        ahc->ahcsrc=gst_bin_get_by_name(GST_BIN(ahc->pipeline), "camera");
        ahc->vfilter1=gst_bin_get_by_name(GST_BIN(ahc->pipeline), "filter1");
        //gst_element_set_state(ahc->pipeline, GST_STATE_PLAYING);
         g_print("\n Playing !!!!!!! \n");

    }



/*
 * gst_rtsp_media_factory_set_launch should be conditioned on the playing state of the pipeline. I think, Study this !
 *
 * */
   // if (ahc->state == GST_STATE_PLAYING)
    gst_rtsp_media_factory_set_launch ( ahc->factory, "(  intervideosrc do-timestamp=true channel=liveling ! videoscale  !  videoconvert ! video/x-raw, framerate=25/1 ! capsfilter name=filter2  ! queue leaky=downstream  ! x264enc name=encoder tune=zerolatency  qp-min=18 qp-max=30 speed-preset=superfast bitrate=800  !  rtph264pay name=pay0 pt=96  openslessrc  ! queue  ! audioconvert ! audio/x-raw, channels=1, depth=16, width=16, rate=16000 ! rtpL16pay name=pay1 pt=11  )");

    //intervideosrc channel=liveling videotestsrc pattern=18
    //gst_rtsp_media_factory_set_launch ( ahc->factory, "( ahcsrc ! videoconvert ! videoscale ! video/x-raw,width=(int)640,height=(int)360,format=(string)I420 ! x264enc tune=zerolatency !  rtph264pay name=pay0 pt=96 )");

    /* notify when our media is ready, This is called whenever someone asks for
   * the media and a new pipeline with our appsrc is created */
   g_signal_connect (ahc->factory, "media-configure", (GCallback) media_configure, ahc);


  if (ahc->native_window) {
    GST_DEBUG ("Native window already received, notifying the vsink about it.");
    gst_video_overlay_set_window_handle (GST_VIDEO_OVERLAY (ahc->vsink),
        (guintptr) ahc->native_window);
  }

  /* Instruct the bus to emit signals for each received message, and connect to the interesting signals */
  bus = gst_element_get_bus (ahc->pipeline);
  bus_source = gst_bus_create_watch (bus);
  g_source_set_callback (bus_source, (GSourceFunc) gst_bus_async_signal_func,
      NULL, NULL);
  g_source_attach (bus_source, context);
  g_source_unref (bus_source);
  g_signal_connect (G_OBJECT (bus), "message::error", G_CALLBACK (on_error),
      ahc);
  g_signal_connect (G_OBJECT (bus), "message::eos", (GCallback) eos_cb, ahc);
  g_signal_connect (G_OBJECT (bus), "message::state-changed",
      (GCallback) state_changed_cb, ahc);
  gst_object_unref (bus);

    gst_rtsp_media_factory_set_shared (ahc->factory, TRUE);
    gst_rtsp_media_factory_set_media_gtype (ahc->factory, TEST_TYPE_RTSP_MEDIA);
    gst_rtsp_media_factory_set_clock (ahc->factory, global_clock);


    /* attach the test factory to the /test url */
    gst_rtsp_mount_points_add_factory (ahc->mounts, "/test", ahc->factory);

    /* don't need the ref to the mapper anymore */
    g_object_unref (ahc->mounts);

    /* attach the server to the default maincontext */
    //gst_rtsp_server_attach (ahc->server, context);
    GError *error=NULL;
    GSource *gsource;
    gsource = gst_rtsp_server_create_source (ahc->server, NULL, &error);
    g_source_attach(gsource, context);



    /* start serving */
    g_print ("stream ready at rtsp://127.0.0.1:8554/test\n");
  /* Create a GLib Main Loop and set it to run */
  GST_DEBUG ("Entering main loop... (GstAhc:%p)", ahc);
  ahc->main_loop = g_main_loop_new (context, FALSE);
  check_initialization_complete (ahc);
  g_main_loop_run (ahc->main_loop);
  GST_DEBUG ("Exited main loop");
  g_main_loop_unref (ahc->main_loop);
  ahc->main_loop = NULL;


    /* Release the request pads from the Tee, and unref them */
//    gst_element_release_request_pad (ahc->tee, tee_srcpad_1);
//    gst_element_release_request_pad (ahc->tee, tee_srcpad_2);
//    gst_object_unref (tee_srcpad_1);
//    gst_object_unref (tee_srcpad_2);



  /* Free resources */
  g_source_unref(gsource);
  g_main_context_unref (context);
  gst_element_set_state (ahc->pipeline, GST_STATE_NULL);
  gst_object_unref (ahc->vsink);
  gst_object_unref (ahc->vfilter1);
  gst_object_unref (ahc->vfilter2);
  gst_object_unref (ahc->ahcsrc);
  gst_object_unref (ahc->pipeline);

  return NULL;
}

/*
 * Java Bindings
 */
void
gst_native_init (JNIEnv * env, jobject thiz)
{
  GstAhc *data = (GstAhc *) g_malloc0 (sizeof (GstAhc));

  SET_CUSTOM_DATA (env, thiz, native_android_camera_field_id, data);
  GST_DEBUG ("Created GstAhc at %p", data);
  data->app = (*env)->NewGlobalRef (env, thiz);
  GST_DEBUG ("Created GlobalRef for app object at %p", data->app);
  pthread_create (&gst_app_thread, NULL, &app_function, data);
}

void
gst_native_finalize (JNIEnv * env, jobject thiz)
{
  GstAhc *data = GET_CUSTOM_DATA (env, thiz, native_android_camera_field_id);

  if (!data)
    return;
  GST_DEBUG ("Quitting main loop...");
  g_main_loop_quit (data->main_loop);
  GST_DEBUG ("Waiting for thread to finish...");
  pthread_join (gst_app_thread, NULL);
  GST_DEBUG ("Deleting GlobalRef at %p", data->app);
  (*env)->DeleteGlobalRef (env, data->app);
  GST_DEBUG ("Freeing GstAhc at %p", data);
  g_free (data);
  SET_CUSTOM_DATA (env, thiz, native_android_camera_field_id, NULL);
  GST_DEBUG ("Done finalizing");
}

void
gst_native_play (JNIEnv * env, jobject thiz)
{
  GstAhc *data = GET_CUSTOM_DATA (env, thiz, native_android_camera_field_id);

  if (!data)
    return;
  GST_DEBUG ("Setting state to PLAYING");
  gst_element_set_state (data->pipeline, GST_STATE_PLAYING);
}

void
gst_native_pause (JNIEnv * env, jobject thiz)
{
  GstAhc *data = GET_CUSTOM_DATA (env, thiz, native_android_camera_field_id);

  if (!data)
    return;
  GST_DEBUG ("Setting state to PAUSED");
  gst_element_set_state (data->pipeline, GST_STATE_PAUSED);
}

jboolean
gst_class_init (JNIEnv * env, jclass klass)
{
  native_android_camera_field_id =
      (*env)->GetFieldID (env, klass, "native_custom_data", "J");
  GST_DEBUG ("The FieldID for the native_custom_data field is %p",
      native_android_camera_field_id);
  on_error_method_id =
      (*env)->GetMethodID (env, klass, "onError", "(Ljava/lang/String;)V");
  GST_DEBUG ("The MethodID for the onError method is %p", on_error_method_id);
  on_gstreamer_initialized_method_id =
      (*env)->GetMethodID (env, klass, "onGStreamerInitialized", "()V");
  GST_DEBUG ("The MethodID for the onGStreamerInitialized method is %p",
      on_gstreamer_initialized_method_id);
  on_state_changed_method_id =
      (*env)->GetMethodID (env, klass, "onStateChanged", "(I)V");
  GST_DEBUG ("The MethodID for the onStateChanged method is %p",
      on_state_changed_method_id);

  if (!native_android_camera_field_id || !on_error_method_id ||
      !on_gstreamer_initialized_method_id || !on_state_changed_method_id) {
    GST_ERROR
        ("The calling class does not implement all necessary interface methods");
    return JNI_FALSE;
  }
  return JNI_TRUE;
}

void
gst_native_surface_init (JNIEnv * env, jobject thiz, jobject surface)
{
  GstAhc *ahc = GET_CUSTOM_DATA (env, thiz, native_android_camera_field_id);

  if (!ahc)
    return;

  GST_DEBUG ("Received surface %p", surface);
  if (ahc->native_window) {
    GST_DEBUG ("Releasing previous native window %p", ahc->native_window);
    ANativeWindow_release (ahc->native_window);
  }
  ahc->native_window = ANativeWindow_fromSurface (env, surface);
  GST_DEBUG ("Got Native Window %p", ahc->native_window);

  if (ahc->vsink) {
    GST_DEBUG
        ("Pipeline already created, notifying the vsink about the native window.");
    gst_video_overlay_set_window_handle (GST_VIDEO_OVERLAY (ahc->vsink),
        (guintptr) ahc->native_window);
  } else {
    GST_DEBUG
        ("Pipeline not created yet, vsink will later be notified about the native window.");
  }

  check_initialization_complete (ahc);
}

void
gst_native_surface_finalize (JNIEnv * env, jobject thiz)
{
  GstAhc *data = GET_CUSTOM_DATA (env, thiz, native_android_camera_field_id);

  if (!data) {
    GST_WARNING ("Received surface finalize but there is no GstAhc. Ignoring.");
    return;
  }
  GST_DEBUG ("Releasing Native Window %p", data->native_window);
  ANativeWindow_release (data->native_window);
  data->native_window = NULL;

  gst_video_overlay_set_window_handle (GST_VIDEO_OVERLAY (data->vsink),
      (guintptr) NULL);
}

void
gst_native_change_resolution (JNIEnv * env, jobject thiz, jint width, jint height)
{
  GstCaps *new_caps1, *new_caps2;
  GstAhc *ahc = GET_CUSTOM_DATA (env, thiz, native_android_camera_field_id);

  if (!ahc)
    return;

  gst_element_set_state (ahc->pipeline, GST_STATE_READY);
/*
  new_caps = gst_caps_new_simple ("video/x-raw",
      "width", G_TYPE_INT, width,
      "height", G_TYPE_INT, height,

      NULL);*/
/*cant change framerate on the go*/
    new_caps1 = gst_caps_new_simple ("video/x-raw",
                                    "width", G_TYPE_INT, width,
                                    "height", G_TYPE_INT, height,

                                    NULL);

  g_object_set (ahc->vfilter1,
      "caps", new_caps1,
      NULL);

    new_caps2 = gst_caps_new_simple ("video/x-raw",
                                     "width", G_TYPE_INT, width,
                                     "height", G_TYPE_INT, height,

                                     "format", G_TYPE_STRING, "I420",
                                     NULL);

    g_object_set (ahc->vfilter2,
                  "caps", new_caps2,
                  NULL);
    gst_caps_unref (new_caps1);
    gst_caps_unref (new_caps2);

  gst_element_set_state (ahc->pipeline, GST_STATE_PAUSED);
}

void
gst_native_set_white_balance (JNIEnv * env, jobject thiz, jint wb_mode)
{
  GstAhc *ahc = GET_CUSTOM_DATA (env, thiz, native_android_camera_field_id);

  if (!ahc)
    return;

  GST_DEBUG ("Setting WB_MODE (%d)", wb_mode);


  g_object_set (ahc->ahcsrc, "white-balance-mode", wb_mode, NULL);
}

void
gst_native_set_rotate_method (JNIEnv * env, jobject thiz, jint method)
{
  GstAhc *ahc = GET_CUSTOM_DATA (env, thiz, native_android_camera_field_id);

  if (!ahc)
    return;
    /*
     * This method is not called from java, write a separate method for setting up camera modes, e.g. audio focus etc.
     * GST_PHOTOGRAPHY_FOCUS_MODE_CONTINUOUS_NORMAL =6*/
  g_object_set (ahc->ahcsrc, "focus-mode", 6 , NULL);
  g_object_set (ahc->vsink, "rotate-method", method, NULL);
  g_print("Setting rotate-method (%d)\n", method) ;
}

static JNINativeMethod native_methods[] = {
  {"nativeInit", "()V", (void *) gst_native_init},
  {"nativeFinalize", "()V", (void *) gst_native_finalize},
  {"nativePlay", "()V", (void *) gst_native_play},
  {"nativePause", "()V", (void *) gst_native_pause},
  {"nativeClassInit", "()Z", (void *) gst_class_init},
  {"nativeSurfaceInit", "(Ljava/lang/Object;)V",
      (void *) gst_native_surface_init},
  {"nativeSurfaceFinalize", "()V",
      (void *) gst_native_surface_finalize},
  {"nativeChangeResolution", "(II)V",
      (void *) gst_native_change_resolution},
  {"nativeSetRotateMethod", "(I)V",
      (void *) gst_native_set_rotate_method},
  {"nativeSetWhiteBalance", "(I)V",
      (void *) gst_native_set_white_balance}
};

jint
JNI_OnLoad (JavaVM * vm, void *reserved)
{
  JNIEnv *env = NULL;

  /* GST_DEBUG can be used to enable gstreamer log on logcat.
   *
   *  setenv ("GST_DEBUG", "*:4,ahc:5,camera-test:5,ahcsrc:5", 1);
   *  setenv ("GST_DEBUG_NO_COLOR", "1", 1);
   */



  GST_DEBUG_CATEGORY_INIT (debug_category, "camera-test", 0,
      "Android Gstreamer Camera test");

  java_vm = vm;

  if ((*vm)->GetEnv (vm, (void **) &env, JNI_VERSION_1_4) != JNI_OK) {
    GST_ERROR ("Could not retrieve JNIEnv");
    return 0;
  }
  jclass klass =
      (*env)->FindClass (env,
      "org/freedesktop/gstreamer/camera/GstAhc");
  (*env)->RegisterNatives (env, klass, native_methods,
      G_N_ELEMENTS (native_methods));

  pthread_key_create (&current_jni_env, detach_current_thread);

  return JNI_VERSION_1_4;
}
