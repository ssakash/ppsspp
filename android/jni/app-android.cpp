// This is generic code that is included in all Android apps that use the
// Native framework by Henrik Rydg�rd (https://github.com/hrydgard/native).

// It calls a set of methods defined in NativeApp.h. These should be implemented
// by your game or app.

#include <jni.h>
#include <android/log.h>
#include <android/native_window_jni.h>
#include <stdlib.h>
#include <stdint.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <queue>

#include "base/basictypes.h"
#include "base/display.h"
#include "base/mutex.h"
#include "base/NativeApp.h"
#include "base/logging.h"
#include "base/timeutil.h"
#include "thread/threadutil.h"
#include "file/zip_read.h"
#include "input/input_state.h"
#include "profiler/profiler.h"
#include "math/math_util.h"
#include "net/resolve.h"
#include "android/jni/native_audio.h"
#include "gfx/gl_common.h"
#include "gfx_es2/gpu_features.h"

#include "Common/GraphicsContext.h"
#include "Common/GL/GLInterfaceBase.h"

#include "app-android.h"

static JNIEnv *jniEnvUI;

struct FrameCommand {
	FrameCommand() {}
	FrameCommand(std::string cmd, std::string prm) : command(cmd), params(prm) {}

	std::string command;
	std::string params;
};

class AndroidEGLGraphicsContext : public GraphicsContext {
public:
	AndroidEGLGraphicsContext() : wnd_(nullptr), gl(nullptr) {}
	bool Init(ANativeWindow *wnd, int desiredBackbufferSizeX, int desiredBackbufferSizeY, int backbufferFormat);
	void Shutdown() override;
	void SwapBuffers() override;
	void SwapInterval(int interval) override {}
	void Resize() {}
	Thin3DContext *CreateThin3DContext() {
		CheckGLExtensions();
		return T3DCreateGLContext();
	}

private:
	ANativeWindow *wnd_;
	cInterfaceBase *gl;
};

bool AndroidEGLGraphicsContext::Init(ANativeWindow *wnd, int backbufferWidth, int backbufferHeight, int backbufferFormat) {
	wnd_ = wnd;
	gl = HostGL_CreateGLInterface();
	if (!gl) {
		ELOG("ERROR: Failed to create GL interface");
		return false;
	}
	ILOG("EGL interface created. Desired backbuffer size: %dx%d", backbufferWidth, backbufferHeight);

	// Apparently we still have to set this through Java through setFixedSize on the bufferHolder for it to take effect...
	gl->SetBackBufferDimensions(backbufferWidth, backbufferHeight);
	gl->SetMode(MODE_DETECT_ES);

	bool use565 = false;
	switch (backbufferFormat) {
	case 4:  // PixelFormat.RGB_565
		use565 = true;
		break;
	}

	if (!gl->Create(wnd, false, use565)) {
		ELOG("EGL creation failed! (use565=%d)", (int)use565);
		// TODO: What do we do now?
		delete gl;
		return false;
	}
	gl->MakeCurrent();
	return true;
}

void AndroidEGLGraphicsContext::Shutdown() {
	gl->ClearCurrent();
	gl->Shutdown();
	delete gl;
	ANativeWindow_release(wnd_);
}

void AndroidEGLGraphicsContext::SwapBuffers() {
	gl->Swap();
}


static recursive_mutex frameCommandLock;
static std::queue<FrameCommand> frameCommands;

std::string systemName;
std::string langRegion;
std::string mogaVersion;

static float left_joystick_x_async;
static float left_joystick_y_async;
static float right_joystick_x_async;
static float right_joystick_y_async;
static float hat_joystick_x_async;
static float hat_joystick_y_async;

static int optimalFramesPerBuffer = 0;
static int optimalSampleRate = 0;
static int sampleRate = 0;
static int framesPerBuffer = 0;
static int androidVersion;
static int deviceType;

// Should only be used for display detection during startup (for config defaults etc)
// This is the ACTUAL display size, not the hardware scaled display size.
static int display_dpi;
static int display_xres;
static int display_yres;
static int backbuffer_format;  // Android PixelFormat enum

static int desiredBackbufferSizeX;
static int desiredBackbufferSizeY;

static jmethodID postCommand;
static jobject nativeActivity;
static volatile bool exitRenderLoop;
static bool renderLoopRunning;

static float dp_xscale = 1.0f;
static float dp_yscale = 1.0f;

InputState input_state;

static bool renderer_inited = false;
static bool first_lost = true;
static std::string library_path;
static std::map<SystemPermission, PermissionStatus> permissions;

AndroidEGLGraphicsContext *graphicsContext;

void PushCommand(std::string cmd, std::string param) {
	lock_guard guard(frameCommandLock);
	frameCommands.push(FrameCommand(cmd, param));
}

// Android implementation of callbacks to the Java part of the app
void SystemToast(const char *text) {
	PushCommand("toast", text);
}

void ShowKeyboard() {
	PushCommand("showKeyboard", "");
}

void Vibrate(int length_ms) {
	char temp[32];
	sprintf(temp, "%i", length_ms);
	PushCommand("vibrate", temp);
}

void LaunchBrowser(const char *url) {
	PushCommand("launchBrowser", url);
}

void LaunchMarket(const char *url) {
	PushCommand("launchMarket", url);
}

void LaunchEmail(const char *email_address) {
	PushCommand("launchEmail", email_address);
}

void System_SendMessage(const char *command, const char *parameter) {
	PushCommand(command, parameter);
}

std::string System_GetProperty(SystemProperty prop) {
	switch (prop) {
	case SYSPROP_NAME:
		return systemName;
	case SYSPROP_LANGREGION:  // "en_US"
		return langRegion;
	case SYSPROP_MOGA_VERSION:
		return mogaVersion;
	default:
		return "";
	}
}

int System_GetPropertyInt(SystemProperty prop) {
	switch (prop) {
	case SYSPROP_SYSTEMVERSION:
		return androidVersion;
	case SYSPROP_DEVICE_TYPE:
		return deviceType;
	case SYSPROP_DISPLAY_XRES:
		return display_xres;
	case SYSPROP_DISPLAY_YRES:
		return display_yres;
	case SYSPROP_AUDIO_SAMPLE_RATE:
		return sampleRate;
	case SYSPROP_AUDIO_FRAMES_PER_BUFFER:
		return framesPerBuffer;
	case SYSPROP_AUDIO_OPTIMAL_SAMPLE_RATE:
		return optimalSampleRate;
	case SYSPROP_AUDIO_OPTIMAL_FRAMES_PER_BUFFER:
		return optimalFramesPerBuffer;
	case SYSPROP_DISPLAY_REFRESH_RATE:
		return (int)(display_hz * 1000.0);
	case SYSPROP_SUPPORTS_PERMISSIONS:
		return androidVersion >= 23;  // 6.0 Marshmallow introduced run time permissions.
	default:
		return -1;
	}
}

std::string GetJavaString(JNIEnv *env, jstring jstr) {
	const char *str = env->GetStringUTFChars(jstr, 0);
	std::string cpp_string = std::string(str);
	env->ReleaseStringUTFChars(jstr, str);
	return cpp_string;
}

extern "C" void Java_org_ppsspp_ppsspp_NativeActivity_registerCallbacks(JNIEnv *env, jobject obj) {
	nativeActivity = env->NewGlobalRef(obj);
	postCommand = env->GetMethodID(env->GetObjectClass(obj), "postCommand", "(Ljava/lang/String;Ljava/lang/String;)V");
	ILOG("Got method ID to postCommand: %p", postCommand);
}

extern "C" void Java_org_ppsspp_ppsspp_NativeActivity_unregisterCallbacks(JNIEnv *env, jobject obj) {
	env->DeleteGlobalRef(nativeActivity);
	nativeActivity = nullptr;
}

// This is now only used as a trigger for GetAppInfo as a function to all before Init.
// On Android we don't use any of the values it returns.
extern "C" jboolean Java_org_ppsspp_ppsspp_NativeApp_isLandscape(JNIEnv *env, jclass) {
	std::string app_name, app_nice_name, version;
	bool landscape;
	NativeGetAppInfo(&app_name, &app_nice_name, &landscape, &version);
	return landscape;
}

// Allow the app to intercept the back button.
extern "C" jboolean Java_org_ppsspp_ppsspp_NativeApp_isAtTopLevel(JNIEnv *env, jclass) {
	return NativeIsAtTopLevel();
}

extern "C" void Java_org_ppsspp_ppsspp_NativeApp_audioConfig
	(JNIEnv *env, jclass, jint optimalFPB, jint optimalSR) {
	optimalFramesPerBuffer = optimalFPB;
	optimalSampleRate = optimalSR;
}

extern "C" jstring Java_org_ppsspp_ppsspp_NativeApp_queryConfig
	(JNIEnv *env, jclass, jstring jquery) {
	std::string query = GetJavaString(env, jquery);
	std::string result = NativeQueryConfig(query);
	jstring jresult = env->NewStringUTF(result.c_str());
	return jresult;
}

extern "C" void Java_org_ppsspp_ppsspp_NativeApp_init
  (JNIEnv *env, jclass, jstring jmodel, jint jdeviceType, jstring jlangRegion, jstring japkpath,
		jstring jdataDir, jstring jexternalDir, jstring jlibraryDir, jstring jcacheDir, jstring jshortcutParam,
		jint jAndroidVersion) {
	jniEnvUI = env;

	setCurrentThreadName("androidInit");

	ILOG("NativeApp.init() -- begin");
	PROFILE_INIT();

	memset(&input_state, 0, sizeof(input_state));
	renderer_inited = false;
	first_lost = true;
	androidVersion = jAndroidVersion;
	deviceType = jdeviceType;

	g_buttonTracker.Reset();

	left_joystick_x_async = 0;
	left_joystick_y_async = 0;
	right_joystick_x_async = 0;
	right_joystick_y_async = 0;
	hat_joystick_x_async = 0;
	hat_joystick_y_async = 0;

	std::string apkPath = GetJavaString(env, japkpath);
	VFSRegister("", new ZipAssetReader(apkPath.c_str(), "assets/"));

	systemName = GetJavaString(env, jmodel);
	langRegion = GetJavaString(env, jlangRegion);

	std::string externalDir = GetJavaString(env, jexternalDir);
	std::string user_data_path = GetJavaString(env, jdataDir) + "/";
	library_path = GetJavaString(env, jlibraryDir) + "/";
	std::string shortcut_param = GetJavaString(env, jshortcutParam);
	std::string cacheDir = GetJavaString(env, jcacheDir);

	ILOG("NativeApp.init(): External storage path: %s", externalDir.c_str());
	ILOG("NativeApp.init(): Launch shortcut parameter: %s", shortcut_param.c_str());

	std::string app_name;
	std::string app_nice_name;
	std::string version;
	bool landscape;

	net::Init();

	NativeGetAppInfo(&app_name, &app_nice_name, &landscape, &version);

	// If shortcut_param is not empty, pass it as additional varargs argument to NativeInit() method.
	// NativeInit() is expected to treat extra argument as boot_filename, which in turn will start game immediately.
	// NOTE: Will only work if ppsspp started from Activity.onCreate(). Won't work if ppsspp app start from onResume().

	if (shortcut_param.empty()) {
		const char *argv[2] = {app_name.c_str(), 0};
		NativeInit(1, argv, user_data_path.c_str(), externalDir.c_str(), cacheDir.c_str());
	}
	else {
		const char *argv[3] = {app_name.c_str(), shortcut_param.c_str(), 0};
		NativeInit(2, argv, user_data_path.c_str(), externalDir.c_str(), cacheDir.c_str());
	}

	ILOG("NativeApp.init() -- end");
}

extern "C" void Java_org_ppsspp_ppsspp_NativeApp_audioInit(JNIEnv *, jclass) {
	sampleRate = optimalSampleRate;
	if (NativeQueryConfig("force44khz") != "0" || optimalSampleRate == 0) {
		sampleRate = 44100;
	}
	if (optimalFramesPerBuffer > 0) {
		framesPerBuffer = optimalFramesPerBuffer;
	} else {
		framesPerBuffer = 512;
	}

	// Some devices have totally bonkers buffer sizes like 8192. They will have terrible latency anyway, so to avoid having to
	// create extra smart buffering code, we'll just let their regular mixer deal with it, missing the fast path (as if they had one...)
	if (framesPerBuffer > 512) {
		framesPerBuffer = 512;
		sampleRate = 44100;
	}

	ILOG("NativeApp.audioInit() -- Using OpenSL audio! frames/buffer: %i   optimal sr: %i   actual sr: %i", optimalFramesPerBuffer, optimalSampleRate, sampleRate);
	AndroidAudio_Init(&NativeMix, library_path, framesPerBuffer, sampleRate);
}

extern "C" void Java_org_ppsspp_ppsspp_NativeApp_audioShutdown(JNIEnv *, jclass) {
	AndroidAudio_Shutdown();
}

extern "C" void Java_org_ppsspp_ppsspp_NativeApp_resume(JNIEnv *, jclass) {
	ILOG("NativeApp.resume() - resuming audio");
	AndroidAudio_Resume();
}

extern "C" void Java_org_ppsspp_ppsspp_NativeApp_pause(JNIEnv *, jclass) {
	ILOG("NativeApp.pause() - pausing audio");
	AndroidAudio_Pause();
}

extern "C" void Java_org_ppsspp_ppsspp_NativeApp_shutdown(JNIEnv *, jclass) {
	ILOG("NativeApp.shutdown() -- begin");
	NativeShutdown();
	VFSShutdown();
	net::Shutdown();
	ILOG("NativeApp.shutdown() -- end");
}

extern "C" void Java_org_ppsspp_ppsspp_NativeRenderer_displayShutdown(JNIEnv *env, jobject obj) {
	if (renderer_inited) {
		renderer_inited = false;
		NativeMessageReceived("recreateviews", "");
	}
}

void System_AskForPermission(SystemPermission permission) {
	switch (permission) {
	case SYSTEM_PERMISSION_STORAGE:
		PushCommand("ask_permission", "storage");
		break;
	}
}

PermissionStatus System_GetPermissionStatus(SystemPermission permission) {
	if (androidVersion < 23) {
		return PERMISSION_STATUS_GRANTED;
	} else {
		return permissions[permission];
	}
}

extern "C" jboolean JNICALL Java_org_ppsspp_ppsspp_NativeApp_touch
	(JNIEnv *, jclass, float x, float y, int code, int pointerId) {
	float scaledX = x * dp_xscale;
	float scaledY = y * dp_yscale;

	TouchInput touch;
	touch.id = pointerId;
	touch.x = scaledX;
	touch.y = scaledY;
	touch.flags = code;
	if (code & 2) {
		input_state.pointer_down[pointerId] = true;
	} else if (code & 4) {
		input_state.pointer_down[pointerId] = false;
	}

	bool retval = NativeTouch(touch);
	{
		lock_guard guard(input_state.lock);
		if (pointerId >= MAX_POINTERS) {
			ELOG("Too many pointers: %i", pointerId);
			return false;	// We ignore 8+ pointers entirely.
		}
		input_state.pointer_x[pointerId] = scaledX;
		input_state.pointer_y[pointerId] = scaledY;
		input_state.mouse_valid = true;
	}
	return retval;
}

extern "C" jboolean Java_org_ppsspp_ppsspp_NativeApp_keyDown(JNIEnv *, jclass, jint deviceId, jint key, jboolean isRepeat) {
	KeyInput keyInput;
	keyInput.deviceId = deviceId;
	keyInput.keyCode = key;
	keyInput.flags = KEY_DOWN;
	if (isRepeat) {
		keyInput.flags |= KEY_IS_REPEAT;
	}
	return NativeKey(keyInput);
}

extern "C" jboolean Java_org_ppsspp_ppsspp_NativeApp_keyUp(JNIEnv *, jclass, jint deviceId, jint key) {
	KeyInput keyInput;
	keyInput.deviceId = deviceId;
	keyInput.keyCode = key;
	keyInput.flags = KEY_UP;
	return NativeKey(keyInput);
}

extern "C" void Java_org_ppsspp_ppsspp_NativeApp_beginJoystickEvent(
	JNIEnv *env, jclass) {
	// mutex lock?
}

extern "C" jboolean Java_org_ppsspp_ppsspp_NativeApp_joystickAxis(
		JNIEnv *env, jclass, jint deviceId, jint axisId, jfloat value) {
	if (!renderer_inited)
		return false;
	switch (axisId) {
	case JOYSTICK_AXIS_X:
		left_joystick_x_async = value;
		break;
	case JOYSTICK_AXIS_Y:
		left_joystick_y_async = -value;
		break;
	case JOYSTICK_AXIS_Z:
		right_joystick_x_async = value;
		break;
	case JOYSTICK_AXIS_RZ:
		right_joystick_y_async = -value;
		break;
	case JOYSTICK_AXIS_HAT_X:
		hat_joystick_x_async = value;
		break;
	case JOYSTICK_AXIS_HAT_Y:
		hat_joystick_y_async = -value;
		break;
	}

	AxisInput axis;
	axis.axisId = axisId;
	axis.deviceId = deviceId;
	axis.value = value;
	return NativeAxis(axis);
}

extern "C" void Java_org_ppsspp_ppsspp_NativeApp_endJoystickEvent(
	JNIEnv *env, jclass) {
	// mutex unlock?
}


extern "C" jboolean Java_org_ppsspp_ppsspp_NativeApp_mouseWheelEvent(
	JNIEnv *env, jclass, jint stick, jfloat x, jfloat y) {
	// TODO: Support mousewheel for android
	return true;
}

extern "C" jboolean JNICALL Java_org_ppsspp_ppsspp_NativeApp_accelerometer(JNIEnv *, jclass, float x, float y, float z) {
	if (!renderer_inited)
		return false;

	// Theoretically this needs locking but I doubt it matters. Worst case, the X
	// from one "sensor frame" will be used together with Y from the next.
	// Should look into quantization though, for compressed movement storage.
	input_state.accelerometer_valid = true;
	input_state.acc.x = x;
	input_state.acc.y = y;
	input_state.acc.z = z;

	AxisInput axis;
	axis.deviceId = DEVICE_ID_ACCELEROMETER;
	axis.flags = 0;

	axis.axisId = JOYSTICK_AXIS_ACCELEROMETER_X;
	axis.value = x;
	bool retvalX = NativeAxis(axis);

	axis.axisId = JOYSTICK_AXIS_ACCELEROMETER_Y;
	axis.value = y;
	bool retvalY = NativeAxis(axis);

	axis.axisId = JOYSTICK_AXIS_ACCELEROMETER_Z;
	axis.value = z;
	bool retvalZ = NativeAxis(axis);

	return retvalX || retvalY || retvalZ;
}

extern "C" void JNICALL Java_org_ppsspp_ppsspp_NativeApp_sendMessage(JNIEnv *env, jclass, jstring message, jstring param) {
	std::string msg = GetJavaString(env, message);
	std::string prm = GetJavaString(env, param);

	// Some messages are caught by app-android.
	if (msg == "moga") {
		mogaVersion = prm;
	} else if (msg == "permission_pending") {
		// TODO: Add support for other permissions
		permissions[SYSTEM_PERMISSION_STORAGE] = PERMISSION_STATUS_PENDING;
		NativePermissionStatus(SYSTEM_PERMISSION_STORAGE, PERMISSION_STATUS_PENDING);
	} else if (msg == "permission_denied") {
		permissions[SYSTEM_PERMISSION_STORAGE] = PERMISSION_STATUS_DENIED;
		NativePermissionStatus(SYSTEM_PERMISSION_STORAGE, PERMISSION_STATUS_PENDING);
	} else if (msg == "permission_granted") {
		permissions[SYSTEM_PERMISSION_STORAGE] = PERMISSION_STATUS_GRANTED;
		NativePermissionStatus(SYSTEM_PERMISSION_STORAGE, PERMISSION_STATUS_PENDING);
	}

	NativeMessageReceived(msg.c_str(), prm.c_str());
}

extern "C" void JNICALL Java_org_ppsspp_ppsspp_NativeActivity_exitEGLRenderLoop(JNIEnv *env, jobject obj) {
	if (!renderLoopRunning) {
		ELOG("Render loop already exited");
		return;
	}
	exitRenderLoop = true;
	while (renderLoopRunning) {
		sleep_ms(10);
	}
}

void correctRatio(int &sz_x, int &sz_y, float scale) {
	float x = (float)sz_x;
	float y = (float)sz_y;
	float ratio = x / y;
	ILOG("CorrectRatio: Considering size: %0.2f/%0.2f=%0.2f for scale %f", x, y, ratio, scale);
	float targetRatio;

	// Try to get the longest dimension to match scale*PSP resolution.
	if (x >= y) {
		targetRatio = 480.0f / 272.0f;
		x = 480.f * scale;
		y = 272.f * scale;
	} else {
		targetRatio = 272.0f / 480.0f;
		x = 272.0f * scale;
		y = 480.0f * scale;
	}

	float correction = targetRatio / ratio;
	ILOG("Target ratio: %0.2f ratio: %0.2f correction: %0.2f", targetRatio, ratio, correction);
	if (ratio < targetRatio) {
		y *= correction;
	} else {
		x /= correction;
	}

	sz_x = x;
	sz_y = y;
	ILOG("Corrected ratio: %dx%d", sz_x, sz_y);
}

void getDesiredBackbufferSize(int &sz_x, int &sz_y) {
	sz_x = display_xres;
	sz_y = display_yres;
	std::string config = NativeQueryConfig("hwScale");
	int scale;
	if (1 == sscanf(config.c_str(), "%d", &scale) && scale > 0) {
		correctRatio(sz_x, sz_y, scale);
	} else {
		sz_x = 0;
		sz_y = 0;
	}
}

extern "C" void JNICALL Java_org_ppsspp_ppsspp_NativeApp_setDisplayParameters(JNIEnv *, jclass, jint xres, jint yres, jint dpi, jfloat refreshRate) {
	ILOG("NativeApp.setDisplayParameters(%d x %d, dpi=%d, refresh=%0.2f)", xres, yres, dpi, refreshRate);
	display_xres = xres;
	display_yres = yres;
	display_dpi = dpi;
	display_hz = refreshRate;
}

extern "C" void JNICALL Java_org_ppsspp_ppsspp_NativeApp_backbufferResize(JNIEnv *, jclass, jint bufw, jint bufh, jint format) {
	ILOG("NativeApp.backbufferResize(%d x %d)", bufw, bufh);

	// pixel_*res is the backbuffer resolution.
	pixel_xres = bufw;
	pixel_yres = bufh;
	backbuffer_format = format;

	g_dpi = (int)display_dpi;
	g_dpi_scale = 240.0f / (float)g_dpi;

	dp_xres = display_xres * g_dpi_scale;
	dp_yres = display_yres * g_dpi_scale;

	// Touch scaling is from display pixels to dp pixels.
	dp_xscale = (float)dp_xres / (float)display_xres;
	dp_yscale = (float)dp_yres / (float)display_yres;

	pixel_in_dps = (float)pixel_xres / dp_xres;

	ILOG("dp_xscale=%f dp_yscale=%f", dp_xscale, dp_yscale);
	ILOG("dp_xres=%d dp_yres=%d", dp_xres, dp_yres);
	ILOG("pixel_xres=%d pixel_yres=%d", pixel_xres, pixel_yres);
	ILOG("g_dpi=%d g_dpi_scale=%f", g_dpi, g_dpi_scale);

	NativeResized();
}

extern "C" void JNICALL Java_org_ppsspp_ppsspp_NativeApp_computeDesiredBackbufferDimensions() {
	getDesiredBackbufferSize(desiredBackbufferSizeX, desiredBackbufferSizeY);
}

extern "C" jint JNICALL Java_org_ppsspp_ppsspp_NativeApp_getDesiredBackbufferWidth(JNIEnv *, jclass) {
	return desiredBackbufferSizeX;
}

extern "C" jint JNICALL Java_org_ppsspp_ppsspp_NativeApp_getDesiredBackbufferHeight(JNIEnv *, jclass) {
	return desiredBackbufferSizeY;
}

void ProcessFrameCommands(JNIEnv *env) {
	lock_guard guard(frameCommandLock);
	while (!frameCommands.empty()) {
		FrameCommand frameCmd;
		frameCmd = frameCommands.front();
		frameCommands.pop();

		WLOG("frameCommand! '%s' '%s'", frameCmd.command.c_str(), frameCmd.params.c_str());

		jstring cmd = env->NewStringUTF(frameCmd.command.c_str());
		jstring param = env->NewStringUTF(frameCmd.params.c_str());
		env->CallVoidMethod(nativeActivity, postCommand, cmd, param);
		env->DeleteLocalRef(cmd);
		env->DeleteLocalRef(param);
	}
}

extern "C" bool JNICALL Java_org_ppsspp_ppsspp_NativeActivity_runEGLRenderLoop(JNIEnv *env, jobject obj, jobject _surf) {
	ANativeWindow *wnd = ANativeWindow_fromSurface(env, _surf);

	WLOG("runEGLRenderLoop. display_xres=%d display_yres=%d", display_xres, display_yres);

	if (wnd == nullptr) {
		ELOG("Error: Surface is null.");
		return false;
	}

	AndroidEGLGraphicsContext *graphicsContext = new AndroidEGLGraphicsContext();
	if (!graphicsContext->Init(wnd, desiredBackbufferSizeX, desiredBackbufferSizeY, backbuffer_format)) {
		ELOG("Failed to initialize graphics context.");
		delete graphicsContext;
		return false;
	}

	if (!renderer_inited) {
		NativeInitGraphics(graphicsContext);
		renderer_inited = true;
	}

	exitRenderLoop = false;
	renderLoopRunning = true;

	while (!exitRenderLoop) {
		static bool hasSetThreadName = false;
		if (!hasSetThreadName) {
			hasSetThreadName = true;
			setCurrentThreadName("AndroidRender");
		}

		// TODO: Look into if these locks are a perf loss
		{
			lock_guard guard(input_state.lock);

			input_state.pad_lstick_x = left_joystick_x_async;
			input_state.pad_lstick_y = left_joystick_y_async;
			input_state.pad_rstick_x = right_joystick_x_async;
			input_state.pad_rstick_y = right_joystick_y_async;

			UpdateInputState(&input_state);
		}
		NativeUpdate(input_state);

		{
			lock_guard guard(input_state.lock);
			EndInputState(&input_state);
		}

		NativeRender(graphicsContext);
		time_update();

		graphicsContext->SwapBuffers();

		ProcessFrameCommands(env);
	}

	// Restore lost device objects. TODO: This feels like the wrong place for this.
	NativeDeviceLost();
	ILOG("NativeDeviceLost completed.");

	NativeShutdownGraphics();
	renderer_inited = false;

	graphicsContext->Shutdown();

	renderLoopRunning = false;
	WLOG("Render loop function exited.");
	return true;
}
