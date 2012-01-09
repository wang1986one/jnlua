/*
 * $Id$
 * See LICENSE.txt for license terms.
 */

#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <jni.h>
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

/* Include uintptr_t */
#ifdef LUA_WIN
#include <stddef.h>
#endif
#ifdef LUA_USE_POSIX
#include <stdint.h>
#endif

/* ---- Definitions ---- */
#define JNLUA_WEAKREF 0
#define JNLUA_HARDREF 1
#define JNLUA_APIVERSION 3
#define JNLUA_MOBJECT "com.naef.jnlua.Object"
#define JNLUA_RENV "com.naef.jnlua.Env"
#define JNLUA_RJAVASTATE "com.naef.jnlua.JavaState"
#define JNLUA_JNIVERSION JNI_VERSION_1_6

/* ---- Types ---- */
/* Structure for reading and writing Java streams. */
typedef struct StreamStruct  {
	JNIEnv *env;
	jobject stream;
	jbyteArray byteArray;
	jbyte* bytes;
	jboolean isCopy;
} Stream;

/* ---- JNI helpers ---- */
static jclass referenceClass(JNIEnv *env, const char *className);
static jobject newGlobalRef(JNIEnv *env, lua_State *luaState, jobject obj, int type);
static jbyteArray newByteArray(JNIEnv *env, lua_State *luaState, jsize length);
static const char *getStringUtfChars(JNIEnv *env, lua_State *luaState, jstring string);

/* ---- Lua helpers ---- */
static void checkstack(lua_State *luaState, int space, const char *msg);

/* ---- Java state operations ---- */
static lua_State *getLuaState(JNIEnv *env, jobject obj);
static void setLuaState(JNIEnv *env, jobject obj, lua_State *luaState);
static lua_State *getLuaThread(JNIEnv *env, jobject obj);
static void setLuaThread(JNIEnv *env, jobject obj, lua_State *luaState);

/* ---- Lua state operations ---- */
static JNIEnv *getJniEnv(lua_State *luaState);
static void setJniEnv(lua_State* luaState, JNIEnv *env);
static jobject getJavaState(lua_State *luaState);
static void setJavaState(lua_State *luaState, jobject javaState);

/* ---- Checks ---- */
static int validindex(lua_State *luaState, int index);
static void checkindex(JNIEnv *env, lua_State *luaState, int index);
static void checkrealindex(JNIEnv *env, lua_State *luaState, int index);
static void checktype(JNIEnv *env, lua_State *luaState, int index, int type);
static void checknelems(JNIEnv *env, lua_State *luaState, int n);
static void checknotnull (JNIEnv *env, lua_State *luaState, void *object);
static void checkarg(JNIEnv *env, lua_State *luaState, int cond, const char *msg);
static void checkstate(JNIEnv *env, lua_State *luaState, int cond, const char *msg);
static void check(JNIEnv *env, lua_State *luaState, int cond, jthrowable throwableClass, const char *msg);
static void throw(JNIEnv *env, lua_State *luaState, jthrowable throwableClass, const char *msg);

/* ---- Java object helpers ---- */
static void pushJavaObject(JNIEnv *env, lua_State *luaState, jobject object);
static jobject getJavaObject(JNIEnv *env, lua_State *luaState, int index, jclass class);
static jstring toString(JNIEnv *env, lua_State *luaState, int index);

/* ---- Metamethods ---- */
static int gcJavaObject(lua_State *luaState);
static int callJavaFunction(lua_State *luaState);

/* ---- Errror handling ---- */
static int handleError(lua_State *luaState);
static int processActivationRecord(lua_Debug *ar);
static void throwException(JNIEnv *env, lua_State *luaState, int status);

/* ---- Stream adapters ---- */
static const char *readInputStream(lua_State *luaState, void *ud, size_t *size);
static int writeOutputStream(lua_State *luaState, const void *data, size_t size, void *ud);

/* ---- Variables ---- */
static jclass luaStateClass = NULL;
static jfieldID luaStateId = 0;
static jfieldID luaThreadId = 0;
static jfieldID yieldId = 0;
static jclass javaFunctionInterface = NULL;
static jmethodID invokeId = 0;
static jclass luaRuntimeExceptionClass = NULL;
static jmethodID luaRuntimeExceptionInitId = 0;
static jmethodID setLuaErrorId = 0;
static jclass luaSyntaxExceptionClass = NULL;
static jmethodID luaSyntaxExceptionInitId = 0;
static jclass luaMemoryAllocationExceptionClass = NULL;
static jmethodID luaMemoryAllocationExceptionInitId = 0;
static jclass luaGcMetamethodExceptionClass = NULL;
static jmethodID luaGcMetamethodExceptionInitId = 0;
static jclass luaMessageHandlerExceptionClass = NULL;
static jmethodID luaMessageHandlerExceptionInitId = 0;
static jclass luaStackTraceElementClass = NULL;
static jmethodID luaStackTraceElementInitId = 0;
static jclass luaErrorClass = NULL;
static jmethodID luaErrorInitId = 0;
static jmethodID setLuaStackTraceId = 0;
static jclass throwableClass = NULL;
static jmethodID getMessageId = 0;
static jclass nullPointerExceptionClass = NULL;
static jclass illegalArgumentExceptionClass = NULL;
static jclass illegalStateExceptionClass = NULL;
static jclass inputStreamClass = NULL;
static jmethodID readId = 0;
static jclass outputStreamClass = NULL;
static jmethodID writeId = 0;
static jclass ioExceptionClass = NULL;
static jclass enumClass = NULL;
static jmethodID nameId = 0;
static int initialized = 0;
static jmp_buf initJumpBuffer;

/* ---- Error handling ---- */
/*
 * JNI does not allow uncontrolled transitions such as jongjmp between Java
 * code and native code, but Lua uses longjmp for error handling. The follwing
 * section replicates logic from luaD_rawrunprotected that is internal to
 * Lua. Contact me if you know of a more elegant solution ;)
 */

struct lua_longjmp {
	struct lua_longjmp *previous;
	jmp_buf b;
	volatile int status;
};

struct lua_State {
	void *next;
	unsigned char tt;
	unsigned char marked;
	unsigned char status;
	void *top;
	void *l_G;
	void *ci;
	void *oldpc;
	void *stack_last;
	void *stack;
	int stacksize;
	unsigned short nny;
	unsigned short nCcalls;  
	unsigned char hookmask;
	unsigned char allowhook;
	int basehookcount;
	int hookcount;
	lua_Hook hook;
	void *openupval;
	void *gclist;
	struct lua_longjmp *errorJmp;  
};

#define JNLUA_TRY {\
	unsigned short oldnCcalls = luaState->nCcalls;\
	struct lua_longjmp lj;\
	lj.status = LUA_OK;\
	lj.previous = luaState->errorJmp;\
	luaState->errorJmp = &lj;\
	if (setjmp(lj.b) == 0) {\
		checkstack(luaState, LUA_MINSTACK, NULL);\
		setJniEnv(luaState, env);
#define JNLUA_END }\
	luaState->errorJmp = lj.previous;\
	luaState->nCcalls = oldnCcalls;\
	if (lj.status != LUA_OK) {\
		throwException(env, luaState, lj.status);\
	}\
}
#define JNLUA_THROW(status) lj.status = status;\
	longjmp(lj.b, -1)

/* ---- Fields ---- */
/* lua_registryindex() */
JNIEXPORT jint JNICALL Java_com_naef_jnlua_LuaState_lua_1registryindex(JNIEnv *env, jobject obj) {
	return (jint) LUA_REGISTRYINDEX;
}

/* lua_version() */
JNIEXPORT jstring JNICALL Java_com_naef_jnlua_LuaState_lua_1version(JNIEnv *env, jobject obj) {
	const char *luaVersion;
	
	luaVersion = LUA_VERSION;
	if (strncmp(luaVersion, "Lua ", 4) == 0) {
		luaVersion += 4;
	}
	return (*env)->NewStringUTF(env, luaVersion); 
}

/* ---- Life cycle ---- */
/*
 * lua_newstate()
 * The function is not reentrant. Non-reentrant use is ensured on the Java side.
 */
JNIEXPORT void JNICALL Java_com_naef_jnlua_LuaState_lua_1newstate (JNIEnv *env, jobject obj, int apiversion, jlong existing) {
	lua_State *luaState;
	int success = 0;
	
	/* Initialized? */
	if (!initialized) {
		return;
	}
	
	/* API version? */
	if (apiversion != JNLUA_APIVERSION) {
		return;
	}

	/* Create Lua state */
	luaState = existing == 0 ? luaL_newstate() : (lua_State *) (uintptr_t) existing;
	if (!luaState) {
		return;
	}

	/* Setup Lua state */
	JNLUA_TRY
		/* Set the Java state in the Lua state. */
		setJavaState(luaState, newGlobalRef(env, luaState, obj, JNLUA_WEAKREF));
		
		/*
		 * Create the meta table for Java objects and leave it on the stack. 
		 * Population will be finished on the Java side.
		 */
		luaL_newmetatable(luaState, JNLUA_MOBJECT);
		lua_pushboolean(luaState, 0);
		lua_setfield(luaState, -2, "__metatable");
		lua_pushcfunction(luaState, gcJavaObject);
		lua_setfield(luaState, -2, "__gc");
		success = 1;
	JNLUA_END
	if (!success) {
		lua_close(luaState);
		return;
	}
	
	/* Set the Lua state in the Java state. */
	setLuaThread(env, obj, luaState);
	setLuaState(env, obj, luaState);
}

/* lua_close() */
JNIEXPORT void JNICALL Java_com_naef_jnlua_LuaState_lua_1close (JNIEnv *env, jobject obj, jboolean ownState) {
	lua_State* luaState;
	lua_State* luaThread;
	lua_Debug ar;

	luaState = getLuaState(env, obj);
	if (ownState) {
		/* Can close? */
		luaThread = getLuaThread(env, obj);
		if (luaState != luaThread || lua_getstack(luaState, 0, &ar)) {
			return;
		}
	}

	/* Unset the Lua state in the Java state. */
	setLuaState(env, obj, NULL);
	setLuaThread(env, obj, NULL);

	if (ownState) {
		/* Release Java state */
		(*env)->DeleteWeakGlobalRef(env, getJavaState(luaState));
		
		/* Close Lua state */
		lua_close(luaState);
	} else {
		/* Detach Lua state. */
		(*env)->DeleteWeakGlobalRef(env, getJavaState(luaState));
		setJavaState(luaState, NULL);
		setJniEnv(luaState, NULL);
	}
}

/* lua_gc() */
JNIEXPORT jint JNICALL Java_com_naef_jnlua_LuaState_lua_1gc (JNIEnv *env, jobject obj, jint what, jint data) {
	lua_State* luaState;
	int result = 0;
	
	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		result = lua_gc(luaState, what, data);
	JNLUA_END
	return (jint) result;
}

/* ---- Registration ---- */
/* lua_openlib() */
JNIEXPORT void JNICALL Java_com_naef_jnlua_LuaState_lua_1openlib (JNIEnv *env, jobject obj, jint lib) {
	lua_State* luaState;
	lua_CFunction openFunc;
	const char *libName;
	
	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		switch (lib) {
		case 0:
			openFunc = luaopen_base;
			libName = "_G";
			break;
		case 1:
			openFunc = luaopen_package;
			libName = LUA_LOADLIBNAME;
			break;
		case 2:
			openFunc = luaopen_coroutine;
			libName = LUA_COLIBNAME;
			break;
		case 3:
			openFunc = luaopen_table;
			libName = LUA_TABLIBNAME;
			break;
		case 4:
			openFunc = luaopen_io;
			libName = LUA_IOLIBNAME;
			break;
		case 5:
  			openFunc = luaopen_os;
  			libName = LUA_OSLIBNAME;
  			break;
  		case 6:
  			openFunc = luaopen_string;
  			libName = LUA_STRLIBNAME;
  			break;
		case 7:
			openFunc = luaopen_bit32;
			libName = LUA_BITLIBNAME;
			break;
  		case 8:
  			openFunc = luaopen_math;
  			libName = LUA_MATHLIBNAME;
  			break;
  		case 9:
  			openFunc = luaopen_debug;
  			libName = LUA_DBLIBNAME;
  			break;
  		default:
  			checkarg(env, luaState, 0, "illegal library");
			return;
  		}
		luaL_requiref(luaState, libName, openFunc, 1);
	JNLUA_END
}

/* ---- Load and dump ---- */
/* lua_load() */
JNIEXPORT void JNICALL Java_com_naef_jnlua_LuaState_lua_1load (JNIEnv *env, jobject obj, jobject inputStream, jstring chunkname, jstring mode) {
	lua_State *luaState;
	const char *chunknameUtf, *modeUtf;
	Stream stream;
	int status;

	chunknameUtf = NULL;
	modeUtf = NULL;
	stream.env = env;
	stream.byteArray = NULL;
	stream.bytes = NULL;
	stream.stream = inputStream;
	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		chunknameUtf = getStringUtfChars(env, luaState, chunkname);
		modeUtf = getStringUtfChars(env, luaState, mode);
		stream.byteArray = newByteArray(env, luaState, 1024);
		status = lua_load(luaState, readInputStream, &stream, chunknameUtf, modeUtf);
		if (status != LUA_OK) {
			JNLUA_THROW(status);
		}
	JNLUA_END
	if (stream.bytes) {
		(*env)->ReleaseByteArrayElements(env, stream.byteArray, stream.bytes, JNI_ABORT);
	}
	if (stream.byteArray) {
		(*env)->DeleteLocalRef(env, stream.byteArray);
	}
	if (chunknameUtf) {
		(*env)->ReleaseStringUTFChars(env, chunkname, chunknameUtf);
	}
	if (modeUtf) {
		(*env)->ReleaseStringUTFChars(env, mode, modeUtf);
	}
}

/* lua_dump() */
JNIEXPORT void JNICALL Java_com_naef_jnlua_LuaState_lua_1dump (JNIEnv *env, jobject obj, jobject outputStream) {
	Stream stream;
	lua_State *luaState;

	stream.env = env;
	stream.byteArray = NULL;
	stream.bytes = NULL;
	stream.stream = outputStream;
	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		stream.byteArray = newByteArray(env, luaState, 1024);
		checknelems(env, luaState, 1);
		lua_dump(luaState, writeOutputStream, &stream);
	JNLUA_END
	if (stream.bytes) {
		(*env)->ReleaseByteArrayElements(env, stream.byteArray, stream.bytes, JNI_ABORT);
	}
	if (stream.byteArray) {
		(*env)->DeleteLocalRef(env, stream.byteArray);
	}
}

/* ---- Call ---- */
/* lua_pcall() */
JNIEXPORT void JNICALL Java_com_naef_jnlua_LuaState_lua_1pcall (JNIEnv *env, jobject obj, jint nargs, jint nresults) {
	lua_State* luaState;
	int index;
	int status;

	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		checkarg(env, luaState, nargs >= 0, "illegal argument count");
		checkarg(env, luaState, nresults >= 0 || nresults == LUA_MULTRET, "illegal return count");
		checknelems(env, luaState, nargs + 1);
		if (nresults != LUA_MULTRET) {
			checkstack(luaState, nresults - (nargs + 1), "call results");
		}
		index = lua_gettop(luaState) - nargs;
		lua_pushcfunction(luaState, handleError);
		lua_insert(luaState, index);
		status = lua_pcall(luaState, nargs, nresults, index);
		lua_remove(luaState, index);
		if (status != LUA_OK) {
			JNLUA_THROW(status);
		}
	JNLUA_END
}

/* ---- Global ---- */
/* lua_getglobal() */
JNIEXPORT void JNICALL Java_com_naef_jnlua_LuaState_lua_1getglobal (JNIEnv *env, jobject obj, jstring name) {
	const char* nameUtf;
	lua_State* luaState;

	nameUtf = NULL;	
	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		nameUtf = getStringUtfChars(env, luaState, name);
		lua_getglobal(luaState, nameUtf);
	JNLUA_END
	if (nameUtf) {
		(*env)->ReleaseStringUTFChars(env, name, nameUtf);
	}
}

/* lua_setglobal() */
JNIEXPORT void JNICALL Java_com_naef_jnlua_LuaState_lua_1setglobal (JNIEnv *env, jobject obj, jstring name) {
	const char* nameUtf;
	lua_State* luaState;

	nameUtf = NULL;	
	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		nameUtf = getStringUtfChars(env, luaState, name);
		checknelems(env, luaState, 1);
		lua_setglobal(luaState, nameUtf);
	JNLUA_END
	if (nameUtf) {
		(*env)->ReleaseStringUTFChars(env, name, nameUtf);
	}
}

/* ---- Stack push ---- */
/* lua_pushboolean() */
JNIEXPORT void JNICALL Java_com_naef_jnlua_LuaState_lua_1pushboolean (JNIEnv *env, jobject obj, jint b) {
	lua_State* luaState;
	
	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		lua_pushboolean(luaState, b);
	JNLUA_END
}

/* lua_pushinteger() */
JNIEXPORT void JNICALL Java_com_naef_jnlua_LuaState_lua_1pushinteger (JNIEnv *env, jobject obj, jint n) {
	lua_State* luaState;
	
	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		lua_pushinteger(luaState, n);
	JNLUA_END
}

/* lua_pushjavafunction() */
JNIEXPORT void JNICALL Java_com_naef_jnlua_LuaState_lua_1pushjavafunction (JNIEnv *env, jobject obj, jobject f) {
	lua_State* luaState;

	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		pushJavaObject(env, luaState, f);
		lua_pushcclosure(luaState, callJavaFunction, 1);
	JNLUA_END
}

/* lua_pushjavaobject() */
JNIEXPORT void JNICALL Java_com_naef_jnlua_LuaState_lua_1pushjavaobject (JNIEnv *env, jobject obj, jobject object) {
	lua_State* luaState;
	
	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		pushJavaObject(env, luaState, object);
	JNLUA_END
}

/* lua_pushnil() */
JNIEXPORT void JNICALL Java_com_naef_jnlua_LuaState_lua_1pushnil (JNIEnv *env, jobject obj) {
	lua_State* luaState;
	
	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		lua_pushnil(luaState);
	JNLUA_END
}

/* lua_pushnumber() */
JNIEXPORT void JNICALL Java_com_naef_jnlua_LuaState_lua_1pushnumber (JNIEnv *env, jobject obj, jdouble n) {
	lua_State* luaState;
	
	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		lua_pushnumber(luaState, n);
	JNLUA_END
}

/* lua_pushstring() */
JNIEXPORT void JNICALL Java_com_naef_jnlua_LuaState_lua_1pushstring (JNIEnv *env, jobject obj, jstring s) {
	const char* sUtf;
	jsize sLength;
	lua_State* luaState;
	
	sUtf = NULL;
	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		sUtf = getStringUtfChars(env, luaState, s);
		sLength = (*env)->GetStringUTFLength(env, s);
		lua_pushlstring(luaState, sUtf, sLength);
	JNLUA_END
	if (sUtf) {
		(*env)->ReleaseStringUTFChars(env, s, sUtf);
	}
}

/* ---- Stack type test ---- */
/* lua_isboolean() */
JNIEXPORT jint JNICALL Java_com_naef_jnlua_LuaState_lua_1isboolean (JNIEnv *env, jobject obj, jint index) {
	lua_State* luaState;
	int result = 0;

	luaState = getLuaThread(env, obj);
	if (!validindex(luaState, index)) {
		return 0;
	}
	JNLUA_TRY
		result = lua_isboolean(luaState, index);
	JNLUA_END
	return (jint) result;
}

/* lua_iscfunction() */
JNIEXPORT jint JNICALL Java_com_naef_jnlua_LuaState_lua_1iscfunction (JNIEnv *env, jobject obj, jint index) {
	lua_State* luaState;
	lua_CFunction cFunction = NULL;
	
	luaState = getLuaThread(env, obj);
	if (!validindex(luaState, index)) {
		return 0;
	}
	JNLUA_TRY
		cFunction = lua_tocfunction(luaState, index);
	JNLUA_END
	return (jint) (cFunction != NULL && cFunction != callJavaFunction);
}

/* lua_isfunction() */
JNIEXPORT jint JNICALL Java_com_naef_jnlua_LuaState_lua_1isfunction (JNIEnv *env, jobject obj, jint index) {
	lua_State* luaState;
	int result = 0;

	luaState = getLuaThread(env, obj);
	if (!validindex(luaState, index)) {
		return 0;
	}
	JNLUA_TRY
		result = lua_isfunction(luaState, index);
	JNLUA_END
	return (jint) result;
}

/* lua_isjavafunction() */
JNIEXPORT jint JNICALL Java_com_naef_jnlua_LuaState_lua_1isjavafunction (JNIEnv *env, jobject obj, jint index) {
	lua_State* luaState;
	int result = 0;
	
	luaState = getLuaThread(env, obj);
	if (!validindex(luaState, index)) {
		return 0;
	}
	JNLUA_TRY
		result = lua_tocfunction(luaState, index) == callJavaFunction;
	JNLUA_END
	return (jint) result;
}

/* lua_isjavaobject() */
JNIEXPORT jint JNICALL Java_com_naef_jnlua_LuaState_lua_1isjavaobject (JNIEnv *env, jobject obj, jint index) {
	lua_State* luaState;
	int result = 0;
	
	luaState = getLuaThread(env, obj);
	if (!validindex(luaState, index)) {
		return 0;
	}
	JNLUA_TRY
		result = getJavaObject(env, luaState, index, 0) != NULL;
	JNLUA_END
	return (jint) result;
}

/* lua_isnil() */
JNIEXPORT jint JNICALL Java_com_naef_jnlua_LuaState_lua_1isnil (JNIEnv *env, jobject obj, jint index) {
	lua_State* luaState;
	int result = 0;

	luaState = getLuaThread(env, obj);
	if (!validindex(luaState, index)) {
		return 0;
	}
	JNLUA_TRY
		result = lua_isnil(luaState, index);
	JNLUA_END
	return (jint) result;
}

/* lua_isnone() */
JNIEXPORT jint JNICALL Java_com_naef_jnlua_LuaState_lua_1isnone (JNIEnv *env, jobject obj, jint index) {
	lua_State* luaState;

	luaState = getLuaThread(env, obj);
	return (jint) !validindex(luaState, index);
}

/* lua_isnoneornil() */
JNIEXPORT jint JNICALL Java_com_naef_jnlua_LuaState_lua_1isnoneornil (JNIEnv *env, jobject obj, jint index) {
	lua_State* luaState;
	int result = 0;

	luaState = getLuaThread(env, obj);
	if (!validindex(luaState, index)) {
		return 1;
	}
	JNLUA_TRY
		result = lua_isnil(luaState, index);
	JNLUA_END
	return (jint) result;
}

/* lua_isnumber() */
JNIEXPORT jint JNICALL Java_com_naef_jnlua_LuaState_lua_1isnumber (JNIEnv *env, jobject obj, jint index) {
	lua_State* luaState;
	int result = 0;

	luaState = getLuaThread(env, obj);
	if (!validindex(luaState, index)) {
		return 0;
	}
	JNLUA_TRY
		result = lua_isnumber(luaState, index);
	JNLUA_END
	return (jint) result;
}

/* lua_isstring() */
JNIEXPORT jint JNICALL Java_com_naef_jnlua_LuaState_lua_1isstring (JNIEnv *env, jobject obj, jint index) {
	lua_State* luaState;
	int result = 0;

	luaState = getLuaThread(env, obj);
	if (!validindex(luaState, index)) {
		return 0;
	}
	JNLUA_TRY
		result = lua_isstring(luaState, index);
	JNLUA_END
	return (jint) result;
}

/* lua_istable() */
JNIEXPORT jint JNICALL Java_com_naef_jnlua_LuaState_lua_1istable (JNIEnv *env, jobject obj, jint index) {
	lua_State* luaState;
	int result = 0;

	luaState = getLuaThread(env, obj);
	if (!validindex(luaState, index)) {
		return 0;
	}
	JNLUA_TRY
		result = lua_istable(luaState, index);
	JNLUA_END
	return (jint) result;
}

/* lua_isthread() */
JNIEXPORT jint JNICALL Java_com_naef_jnlua_LuaState_lua_1isthread (JNIEnv *env, jobject obj, jint index) {
	lua_State* luaState;
	int result = 0;

	luaState = getLuaThread(env, obj);
	if (!validindex(luaState, index)) {
		return 0;
	}
	JNLUA_TRY
		result = lua_isthread(luaState, index);
	JNLUA_END
	return (jint) result;
}

/* ---- Stack query ---- */
/* lua_compare() */
JNIEXPORT jint JNICALL Java_com_naef_jnlua_LuaState_lua_1compare (JNIEnv *env, jobject obj, jint index1, jint index2, jint operator) {
	lua_State* luaState;
	int result = 0;
	
	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		result = lua_compare(luaState, index1, index2, operator);
	JNLUA_END
	return (jint) result;
}

/* lua_rawequal() */
JNIEXPORT jint JNICALL Java_com_naef_jnlua_LuaState_lua_1rawequal (JNIEnv *env, jobject obj, jint index1, jint index2) {
	lua_State* luaState;
	int result = 0;
	
	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		checkindex(env, luaState, index1);
		checkindex(env, luaState, index2);
		result = lua_rawequal(luaState, index1, index2);
	JNLUA_END
	return (jint) result;
}

/* lua_rawlen() */
JNIEXPORT jint JNICALL Java_com_naef_jnlua_LuaState_lua_1rawlen (JNIEnv *env, jobject obj, jint index) {
	lua_State* luaState;
	size_t result = 0;
	
	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		checkindex(env, luaState, index);
		result = lua_rawlen(luaState, index);
	JNLUA_END
	return (jint) result;
}

/* lua_toboolean() */
JNIEXPORT jint JNICALL Java_com_naef_jnlua_LuaState_lua_1toboolean (JNIEnv *env, jobject obj, jint index) {
	lua_State* luaState;
	int result = 0;
	
	luaState = getLuaThread(env, obj);
	if (!validindex(luaState, index)) {
		return 0;
	}
	JNLUA_TRY
		result = lua_toboolean(luaState, index);
	JNLUA_END
	return (jint) result;
}

/* lua_tointeger() */
JNIEXPORT jint JNICALL Java_com_naef_jnlua_LuaState_lua_1tointeger (JNIEnv *env, jobject obj, jint index) {
	lua_State* luaState;
	lua_Integer	result = 0;
	
	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		checkindex(env, luaState, index);
		result = lua_tointeger(luaState, index);
	JNLUA_END
	return (jint) result;
}

/* lua_tojavafunction() */
JNIEXPORT jobject JNICALL Java_com_naef_jnlua_LuaState_lua_1tojavafunction (JNIEnv *env, jobject obj, jint index) {
	lua_State* luaState;
	jobject functionObj = NULL;
	
	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		checkindex(env, luaState, index);
		if (lua_tocfunction(luaState, index) != callJavaFunction) {
			return NULL;
		}
		if (!lua_getupvalue(luaState, index, 1)) {
			return NULL;
		}
		functionObj = getJavaObject(env, luaState, -1, javaFunctionInterface);
		lua_pop(luaState, 1);
	JNLUA_END
	return functionObj;
}

/* lua_tojavaobject() */
JNIEXPORT jobject JNICALL Java_com_naef_jnlua_LuaState_lua_1tojavaobject (JNIEnv *env, jobject obj, jint index) {
	lua_State* luaState;
	jobject result = NULL;
	
	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		checkindex(env, luaState, index);
		result = getJavaObject(env, luaState, index, 0);
	JNLUA_END
	return result;
}

/* lua_tonumber() */
JNIEXPORT jdouble JNICALL Java_com_naef_jnlua_LuaState_lua_1tonumber (JNIEnv *env, jobject obj, jint index) {
	lua_State* luaState;
	lua_Number result = 0.0;
	
	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		checkindex(env, luaState, index);
		result = lua_tonumber(luaState, index);
	JNLUA_END
	return (jdouble) result;
}

/* lua_topointer() */
JNIEXPORT jlong JNICALL Java_com_naef_jnlua_LuaState_lua_1topointer (JNIEnv *env, jobject obj, jint index) {
	lua_State* luaState;
	const void *result = NULL;
	
	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		checkindex(env, luaState, index);
		result = lua_topointer(luaState, index);
	JNLUA_END
	return (jlong) (uintptr_t) result;
}

/* lua_tostring() */
JNIEXPORT jstring JNICALL Java_com_naef_jnlua_LuaState_lua_1tostring (JNIEnv *env, jobject obj, jint index) {
	lua_State* luaState;
	const char* string = NULL;
	
	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		checkindex(env, luaState, index);
		string = lua_tostring(luaState, index);
	JNLUA_END
	return string != NULL ? (*env)->NewStringUTF(env, string) : NULL;
}

/* lua_type() */
JNIEXPORT jint JNICALL Java_com_naef_jnlua_LuaState_lua_1type (JNIEnv *env, jobject obj, jint index) {
	lua_State* luaState;
	int result = 0;
	
	luaState = getLuaThread(env, obj);
	if (!validindex(luaState, index)) {
		return LUA_TNONE;
	}
	JNLUA_TRY
		result = lua_type(luaState, index);
	JNLUA_END
	return (jint) result;
}

/* ---- Stack operations ---- */
/* lua_absindex() */
JNIEXPORT jint JNICALL Java_com_naef_jnlua_LuaState_lua_1absindex (JNIEnv *env, jobject obj, jint index) {
	lua_State* luaState;
	int result = 0;
	
	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		result = lua_absindex(luaState, index);
	JNLUA_END
	return (jint) result;
}

/* lua_arith() */
JNIEXPORT void JNICALL Java_com_naef_jnlua_LuaState_lua_1arith (JNIEnv *env, jobject obj, jint operator) {
	lua_State* luaState;
	
	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		checknelems(env, luaState, operator != LUA_OPUNM ? 2 : 1);
		lua_arith(luaState, operator);
	JNLUA_END
}

/* lua_concat() */
JNIEXPORT void JNICALL Java_com_naef_jnlua_LuaState_lua_1concat (JNIEnv *env, jobject obj, jint n) {
	lua_State* luaState;

	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		checkarg(env, luaState, n >= 0, "illegal count");
		checknelems(env, luaState, n);
		lua_concat(luaState, n);
	JNLUA_END
}

/* lua_copy() */
JNIEXPORT void JNICALL Java_com_naef_jnlua_LuaState_lua_1copy (JNIEnv *env, jobject obj, jint fromIndex, jint toIndex) {
	lua_State* luaState;
	
	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		checkindex(env, luaState, fromIndex);
		checkindex(env, luaState, toIndex);
		lua_copy(luaState, fromIndex, toIndex);
	JNLUA_END
}

/* lua_gettop() */
JNIEXPORT jint JNICALL Java_com_naef_jnlua_LuaState_lua_1gettop (JNIEnv *env, jobject obj) {
	lua_State* luaState;
	int result = 0;
	
	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		result = lua_gettop(luaState);
	JNLUA_END
	return (jint) result;
}

/* lua_len() */
JNIEXPORT void JNICALL Java_com_naef_jnlua_LuaState_lua_1len (JNIEnv *env, jobject obj, jint index) {
	lua_State* luaState;
	
	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		checkindex(env, luaState, index);
		lua_len(luaState, index);
	JNLUA_END
}

/* lua_insert() */
JNIEXPORT void JNICALL Java_com_naef_jnlua_LuaState_lua_1insert (JNIEnv *env, jobject obj, jint index) {
	lua_State* luaState;
	
	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		checkrealindex(env, luaState, index);
		lua_insert(luaState, index);
	JNLUA_END
}

/* lua_pop() */
JNIEXPORT void JNICALL Java_com_naef_jnlua_LuaState_lua_1pop (JNIEnv *env, jobject obj, jint index) {
	lua_State* luaState;
	
	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		checkarg(env, luaState, index >= 0 && index <= lua_gettop(luaState), "illegal count");
		lua_pop(luaState, index);
	JNLUA_END
}

/* lua_pushvalue() */
JNIEXPORT void JNICALL Java_com_naef_jnlua_LuaState_lua_1pushvalue (JNIEnv *env, jobject obj, jint index) {
	lua_State* luaState;
	
	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		checkindex(env, luaState, index);
		lua_pushvalue(luaState, index);
	JNLUA_END
}

/* lua_remove() */
JNIEXPORT void JNICALL Java_com_naef_jnlua_LuaState_lua_1remove (JNIEnv *env, jobject obj, jint index) {
	lua_State* luaState;

	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		checkrealindex(env, luaState, index);
		lua_remove(luaState, index);
	JNLUA_END
}

/* lua_replace() */
JNIEXPORT void JNICALL Java_com_naef_jnlua_LuaState_lua_1replace (JNIEnv *env, jobject obj, jint index) {
	lua_State* luaState;
	
	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		checkindex(env, luaState, index);
		checknelems(env, luaState, 1);
		lua_replace(luaState, index);
	JNLUA_END
}

/* lua_settop() */
JNIEXPORT void JNICALL Java_com_naef_jnlua_LuaState_lua_1settop (JNIEnv *env, jobject obj, jint index) {
	lua_State* luaState;
	
	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		checkarg(env, luaState, index > 0 || (index <= 0 && -index <= lua_gettop(luaState)), "illegal index");
		lua_settop(luaState, index);
	JNLUA_END
}

/* ---- Table ---- */
/* lua_createtable() */
JNIEXPORT void JNICALL Java_com_naef_jnlua_LuaState_lua_1createtable (JNIEnv *env, jobject obj, jint narr, jint nrec) {
	lua_State* luaState;
	
	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		checkarg(env, luaState, narr >= 0, "illegal array count");
		checkarg(env, luaState, nrec >= 0, "illegal record count");
		lua_createtable(luaState, narr, nrec);
	JNLUA_END
}

/* lua_getsubtable() */
JNIEXPORT jint JNICALL Java_com_naef_jnlua_LuaState_lua_1getsubtable (JNIEnv *env, jobject obj, jint index, jstring fname) {
	const char* fnameUtf;
	lua_State *luaState;
	int result = 0;
	
	fnameUtf = NULL;
	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		checkindex(env, luaState, index);
		fnameUtf = getStringUtfChars(env, luaState, fname);
		result = luaL_getsubtable(luaState, index, fnameUtf);
	JNLUA_END
	if (fnameUtf) {
		(*env)->ReleaseStringUTFChars(env, fname, fnameUtf);
	}
	return (jint) result;
}

/* lua_getfield() */
JNIEXPORT void JNICALL Java_com_naef_jnlua_LuaState_lua_1getfield (JNIEnv *env, jobject obj, jint index, jstring k) {
	const char* kUtf;
	lua_State* luaState;
	
	kUtf = NULL;
	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		checktype(env, luaState, index, LUA_TTABLE);
		kUtf = getStringUtfChars(env, luaState, k);
		lua_getfield(luaState, index, kUtf);
	JNLUA_END
	if (kUtf) {
		(*env)->ReleaseStringUTFChars(env, k, kUtf);
	}
}

/* lua_gettable() */
JNIEXPORT void JNICALL Java_com_naef_jnlua_LuaState_lua_1gettable (JNIEnv *env, jobject obj, jint index) {
	lua_State* luaState;
	
	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		checktype(env, luaState, index, LUA_TTABLE);
		lua_gettable(luaState, index);
	JNLUA_END
}

/* lua_newtable() */
JNIEXPORT void JNICALL Java_com_naef_jnlua_LuaState_lua_1newtable (JNIEnv *env, jobject obj) {
	lua_State* luaState;
	
	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		lua_newtable(luaState);
	JNLUA_END
}

/* lua_next() */
JNIEXPORT jint JNICALL Java_com_naef_jnlua_LuaState_lua_1next (JNIEnv *env, jobject obj, jint index) {
	lua_State* luaState;
	int result = 0;
	
	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		checktype(env, luaState, index, LUA_TTABLE);
		checknelems(env, luaState, 1);
		result = lua_next(luaState, index);
	JNLUA_END
	return (jint) result;
}

/* lua_rawget() */
JNIEXPORT void JNICALL Java_com_naef_jnlua_LuaState_lua_1rawget (JNIEnv *env, jobject obj, jint index) {
	lua_State* luaState;
	
	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		checktype(env, luaState, index, LUA_TTABLE);
		checknelems(env, luaState, 1);
		lua_rawget(luaState, index);
	JNLUA_END
}

/* lua_rawgeti() */
JNIEXPORT void JNICALL Java_com_naef_jnlua_LuaState_lua_1rawgeti (JNIEnv *env, jobject obj, jint index, jint n) {
	lua_State* luaState;
	
	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		checktype(env, luaState, index, LUA_TTABLE);
		lua_rawgeti(luaState, index, n);
	JNLUA_END
}

/* lua_rawset() */
JNIEXPORT void JNICALL Java_com_naef_jnlua_LuaState_lua_1rawset (JNIEnv *env, jobject obj, jint index) {
	lua_State* luaState;
	
	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		checktype(env, luaState, index, LUA_TTABLE);
		checknelems(env, luaState, 2);
		lua_rawset(luaState, index);
	JNLUA_END
}

/* lua_rawseti() */
JNIEXPORT void JNICALL Java_com_naef_jnlua_LuaState_lua_1rawseti (JNIEnv *env, jobject obj, jint index, jint n) {
	lua_State* luaState;
	
	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		checktype(env, luaState, index, LUA_TTABLE);
		checknelems(env, luaState, 1);
		lua_rawseti(luaState, index, n);
	JNLUA_END
}

/* lua_settable() */
JNIEXPORT void JNICALL Java_com_naef_jnlua_LuaState_lua_1settable (JNIEnv *env, jobject obj, jint index) {
	lua_State* luaState;
	
	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		checktype(env, luaState, index, LUA_TTABLE);
		checknelems(env, luaState, 2);
		lua_settable(luaState, index);
	JNLUA_END
}

/* lua_setfield() */
JNIEXPORT void JNICALL Java_com_naef_jnlua_LuaState_lua_1setfield (JNIEnv *env, jobject obj, jint index, jstring k) {
	const char* kUtf;
	lua_State* luaState;
	
	kUtf = NULL;
	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		checktype(env, luaState, index, LUA_TTABLE);
		checknelems(env, luaState, 1);
		kUtf = getStringUtfChars(env, luaState, k);
		lua_setfield(luaState, index, kUtf);
	JNLUA_END
	if (kUtf) {
		(*env)->ReleaseStringUTFChars(env, k, kUtf);
	}
}

/* ---- Meta table ---- */
/* lua_getmetatable() */
JNIEXPORT int JNICALL Java_com_naef_jnlua_LuaState_lua_1getmetatable (JNIEnv *env, jobject obj, jint index) {
	lua_State* luaState;
	int result = 0;
	
	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		checkindex(env, luaState, index);
		result = lua_getmetatable(luaState, index);
	JNLUA_END
	return (jint) result;
}

/* lua_setmetatable() */
JNIEXPORT jint JNICALL Java_com_naef_jnlua_LuaState_lua_1setmetatable (JNIEnv *env, jobject obj, jint index) {
	lua_State* luaState;
	int result = 0;
	
	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		checkindex(env, luaState, index);
		checknelems(env, luaState, 1);
		checkarg(env, luaState, lua_type(luaState, -1) == LUA_TTABLE || lua_type(luaState, -1) == LUA_TNIL, "illegal type");
		result = lua_setmetatable(luaState, index);
	JNLUA_END
	return (jint) result;
}

/* lua_getmetafield() */
JNIEXPORT jint JNICALL Java_com_naef_jnlua_LuaState_lua_1getmetafield (JNIEnv *env, jobject obj, jint index, jstring k) {
	const char* kUtf;
	lua_State* luaState;
	int result = 0;
	
	kUtf = NULL;
	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		checkindex(env, luaState, index);
		kUtf = getStringUtfChars(env, luaState, k);
		result = luaL_getmetafield(luaState, index, kUtf);
	JNLUA_END
	if (kUtf) {
		(*env)->ReleaseStringUTFChars(env, k, kUtf);
	}
	return (jint) result;
}

/* ---- Thread ---- */
/* lua_newthread() */
JNIEXPORT void JNICALL Java_com_naef_jnlua_LuaState_lua_1newthread (JNIEnv *env, jobject obj) {
	lua_State *luaState;
	lua_State *luaThread;
	
	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		checktype(env, luaState, -1, LUA_TFUNCTION);
		luaThread = lua_newthread(luaState);
		lua_insert(luaState, -2);
		lua_xmove(luaState, luaThread, 1);
	JNLUA_END
}

/* lua_resume() */
JNIEXPORT jint JNICALL Java_com_naef_jnlua_LuaState_lua_1resume (JNIEnv *env, jobject obj, jint index, jint nargs) {
	lua_State *luaState;
	lua_State *luaThread;
	int status;
	int nresults = 0;
	
	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		checktype(env, luaState, index, LUA_TTHREAD);
		checkarg(env, luaState, nargs >= 0, "illegal argument count");
		checknelems(env, luaState, nargs + 1);
		luaThread = lua_tothread(luaState, index);
		checkstack(luaThread, nargs, "resume arguments");
		lua_xmove(luaState, luaThread, nargs);
		status = lua_resume(luaThread, luaState, nargs);
		switch (status) {
		case LUA_OK:
		case LUA_YIELD:
			nresults = lua_gettop(luaThread);
			checkstack(luaState, nresults, "yield arguments");
			lua_xmove(luaThread, luaState, nresults);
			break;
		default:
			JNLUA_THROW(status);
			nresults = 0;
		}
	JNLUA_END
	return (jint) nresults;
}

/* lua_status() */
JNIEXPORT jint JNICALL Java_com_naef_jnlua_LuaState_lua_1status (JNIEnv *env, jobject obj, jint index) {
	lua_State *luaState;
	lua_State *luaThread;
	int result = 0;
	
	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		checktype(env, luaState, index, LUA_TTHREAD);
		luaThread = lua_tothread(luaState, index);
		result = lua_status(luaThread);
	JNLUA_END
	return (jint) result;	
}

/* ---- Reference ---- */
/* lua_ref() */
JNIEXPORT jint JNICALL Java_com_naef_jnlua_LuaState_lua_1ref (JNIEnv *env, jobject obj, jint index) {
	lua_State* luaState;
	int result = 0;
	
	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		checktype(env, luaState, index, LUA_TTABLE);
		result = luaL_ref(luaState, index);
	JNLUA_END
	return (jint) result;
}

/* lua_unref() */
JNIEXPORT void JNICALL Java_com_naef_jnlua_LuaState_lua_1unref (JNIEnv *env, jobject obj, jint index, jint ref) {
	lua_State* luaState;
	
	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		checktype(env, luaState, index, LUA_TTABLE);
		luaL_unref(luaState, index, ref);
	JNLUA_END
}

/* ---- Optimization ---- */
/* lua_tablesize() */
JNIEXPORT jint JNICALL Java_com_naef_jnlua_LuaState_lua_1tablesize (JNIEnv *env, jobject obj, jint index) {
	lua_State* luaState;
	int count = 0;
	
	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		checktype(env, luaState, index, LUA_TTABLE);
		lua_pushvalue(luaState, index);
		lua_pushnil(luaState);
		count = 0;
		while (lua_next(luaState, -2)) {
			lua_pop(luaState, 1);
			count++;
		}
		lua_pop(luaState, 1);
	JNLUA_END
	return (jint) count;
}

/* lua_tablemove() */
JNIEXPORT void JNICALL Java_com_naef_jnlua_LuaState_lua_1tablemove (JNIEnv *env, jobject obj, jint index, jint from, jint to, jint count) {
	lua_State* luaState;
	int i;
	
	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		checktype(env, luaState, index, LUA_TTABLE);
		checkarg(env, luaState, count >= 0, "illegal count");
		lua_pushvalue(luaState, index);
		if (from < to) {
			for (i = count - 1; i >= 0; i--) {
				lua_rawgeti(luaState, -1, from + i);
				lua_rawseti(luaState, -2, to + i);
			}
		} else if (from > to) {
			for (i = 0; i < count; i++) { 
				lua_rawgeti(luaState, -1, from + i);
				lua_rawseti(luaState, -2, to + i);
			}
		}
		lua_pop(luaState, 1); 
	JNLUA_END
}

/* ---- Argument checking ---- */
/* lua_argcheck() */
JNIEXPORT void JNICALL Java_com_naef_jnlua_LuaState_lua_1argcheck (JNIEnv *env, jobject obj, jboolean cond, jint narg, jstring extraMsg) {
	lua_State *luaState;
	const char *extraMsgUtf;
	
	extraMsgUtf = NULL;
	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		extraMsgUtf = getStringUtfChars(env, luaState, extraMsg);
		luaL_argcheck(luaState, cond, narg, extraMsgUtf);
	JNLUA_END
	if (extraMsgUtf) {
		(*env)->ReleaseStringUTFChars(env, extraMsg, extraMsgUtf);
	}
}

/* lua_checkenum() */
JNIEXPORT jobject JNICALL Java_com_naef_jnlua_LuaState_lua_1checkenum (JNIEnv *env, jobject obj, jint narg, jobject def, jobjectArray lst) {
	lua_State *luaState;
	jstring defString;
	const char *defUtf;
	jsize lstLength, i;
	jstring *lstString;
	const char **lstUtf;
	jobject result = NULL;

	defString = NULL;	
	defUtf = NULL;
	lstLength = 0;
	lstString = NULL;
	lstUtf = NULL;
	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		if (def != NULL) {
			defString = (*env)->CallObjectMethod(env, def, nameId);
			defUtf = getStringUtfChars(env, luaState, defString);
		}
		checknotnull(env, luaState, lst);
		lstLength = (*env)->GetArrayLength(env, lst);
		lstString = (jstring *) calloc(lstLength + 1, sizeof(jstring));
		lstUtf = (const char **) calloc(lstLength + 1, sizeof(const char *));
		check(env, luaState, lstString != NULL && lstUtf != NULL, luaMemoryAllocationExceptionClass, "JNI error: calloc() failed");
		for (i = 0; i < lstLength; i++) {
			lstString[i] = (*env)->CallObjectMethod(env, (*env)->GetObjectArrayElement(env, lst, i), nameId);
			lstUtf[i] = getStringUtfChars(env, luaState, lstString[i]);
		}
		result = (*env)->GetObjectArrayElement(env, lst, luaL_checkoption(luaState, narg, defUtf, lstUtf));
	JNLUA_END
	if (lstUtf) {
		for (i = 0; i < lstLength; i++) {
			if (lstUtf[i]) {
				(*env)->ReleaseStringUTFChars(env, lstString[i], lstUtf[i]);
			}
		}
		free((void *) lstUtf);
	}
	if (lstString) {
		free((void *) lstString);
	}
	if (defUtf) {
		(*env)->ReleaseStringUTFChars(env, defString, defUtf);
	}
	return result;
}

/* lua_checkinteger() */
JNIEXPORT jint JNICALL Java_com_naef_jnlua_LuaState_lua_1checkinteger (JNIEnv *env, jobject obj, jint narg) {
	lua_State *luaState;
	lua_Integer result = 0;
	
	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		result = luaL_checkinteger(luaState, narg);
	JNLUA_END
	return (jint) result;
}

/* lua_checknumber() */
JNIEXPORT jdouble JNICALL Java_com_naef_jnlua_LuaState_lua_1checknumber (JNIEnv *env, jobject obj, jint narg) {
	lua_State *luaState;
	lua_Number result = 0;
	
	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		result = luaL_checknumber(luaState, narg);
	JNLUA_END
	return (jdouble) result;
}

/* lua_checkoption() */
JNIEXPORT jint JNICALL Java_com_naef_jnlua_LuaState_lua_1checkoption (JNIEnv *env, jobject obj, jint narg, jstring def, jobjectArray lst) {
	lua_State *luaState;
	const char *defUtf;
	jsize lstLength, i;
	jstring *lstString;
	const char **lstUtf;
	int result = 0;
	
	defUtf = NULL;
	lstLength = 0;
	lstString = NULL;
	lstUtf = NULL;
	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		if (def != NULL) {
			defUtf = getStringUtfChars(env, luaState, def);
		}
		checknotnull(env, luaState, lst);
		lstLength = (*env)->GetArrayLength(env, lst);
		lstString = (jstring *) calloc(lstLength + 1, sizeof(jstring));
		lstUtf = (const char **) calloc(lstLength + 1, sizeof(const char *));
		check(env, luaState, lstString != NULL && lstUtf != NULL, luaMemoryAllocationExceptionClass, "JNI error: calloc() failed");
		for (i = 0; i < lstLength; i++) {
			lstString[i] = (*env)->GetObjectArrayElement(env, lst, i);
			lstUtf[i] = getStringUtfChars(env, luaState, lstString[i]);
		}
		result = luaL_checkoption(luaState, narg, defUtf, lstUtf);
	JNLUA_END
	if (lstUtf) {
		for (i = 0; i < lstLength; i++) {
			if (lstUtf[i]) {
				(*env)->ReleaseStringUTFChars(env, lstString[i], lstUtf[i]);
			}
		}
		free((void *) lstUtf);
	}
	if (lstString) {
		free((void *) lstString);
	}
	if (defUtf) {
		(*env)->ReleaseStringUTFChars(env, def, defUtf);
	}
	return (jint) result;
}

/* lua_checkstring() */
JNIEXPORT jstring JNICALL Java_com_naef_jnlua_LuaState_lua_1checkstring (JNIEnv *env, jobject obj, jint narg) {
	lua_State *luaState;
	const char *result = NULL;
	
	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		result = luaL_checkstring(luaState, narg);
	JNLUA_END
	return (*env)->NewStringUTF(env, result); 
}

/* lua_checktype() */
JNIEXPORT void JNICALL Java_com_naef_jnlua_LuaState_lua_1checktype (JNIEnv *env, jobject obj, jint narg, jint type) {
	lua_State *luaState;
	
	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		luaL_checktype(luaState, narg, type);
	JNLUA_END
}

/* lua_optinteger() */
JNIEXPORT jint JNICALL Java_com_naef_jnlua_LuaState_lua_1optinteger (JNIEnv *env, jobject obj, jint narg, jint d) {
	lua_State *luaState;
	lua_Integer result = 0;
	
	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		result = luaL_optinteger(luaState, narg, d);
	JNLUA_END
	return (jint) result;
}

/* lua_optnumber() */
JNIEXPORT jdouble JNICALL Java_com_naef_jnlua_LuaState_lua_1optnumber (JNIEnv *env, jobject obj, jint narg, jdouble d) {
	lua_State *luaState;
	lua_Number result = 0;
	
	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		result = luaL_optnumber(luaState, narg, d);
	JNLUA_END
	return (jdouble) result;
}

/* lua_optstring() */
JNIEXPORT jstring JNICALL Java_com_naef_jnlua_LuaState_lua_1optstring (JNIEnv *env, jobject obj, jint narg, jstring d) {
	lua_State *luaState;
	const char *dUtf;
	const char *string;
	jstring result = NULL;
	
	dUtf = NULL;
	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		dUtf = getStringUtfChars(env, luaState, d);
		string = luaL_optstring(luaState, narg, dUtf);
		result = string != dUtf ? (*env)->NewStringUTF(env, string) : d;
	JNLUA_END
	if (dUtf) {
		(*env)->ReleaseStringUTFChars(env, d, dUtf);
	}
	return result;
}

/* ---- Function arguments ---- */
/* Returns the current function name. */
JNIEXPORT jstring JNICALL Java_com_naef_jnlua_LuaState_lua_1funcname (JNIEnv *env, jobject obj) {
	lua_State* luaState;
	lua_Debug ar;
	const char *result = NULL;
	
	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		if (!lua_getstack(luaState, 0, &ar)) {
			return NULL;
		}
		lua_getinfo(luaState, "n", &ar);
		result = ar.name;
	JNLUA_END
	return result != NULL ? (*env)->NewStringUTF(env, ar.name) : NULL;
}

/* Returns the effective argument number, adjusting for methods. */
JNIEXPORT jint JNICALL Java_com_naef_jnlua_LuaState_lua_1narg (JNIEnv *env, jobject obj, jint narg) {
	lua_State* luaState;
	lua_Debug ar;
	
	luaState = getLuaThread(env, obj);
	JNLUA_TRY
		if (lua_getstack(luaState, 0, &ar)) {
			lua_getinfo(luaState, "n", &ar);
			if (strcmp(ar.namewhat, "method") == 0) {
				narg--;
			}
		}
	JNLUA_END
	return narg;
}

/* ---- JNI ---- */
/* Handles the loading of this library. */
JNIEXPORT jint JNICALL JNI_OnLoad (JavaVM *vm, void *reserved) {
	JNIEnv *env;
	
	/* Get environment */
	if ((*vm)->GetEnv(vm, (void **) &env, JNLUA_JNIVERSION) != JNI_OK) {
		return JNLUA_JNIVERSION;
	}

	/* Lookup and pin classes, fields and methods */
	if (!(luaStateClass = referenceClass(env, "com/naef/jnlua/LuaState")) ||
			!(luaStateId = (*env)->GetFieldID(env, luaStateClass, "luaState", "J")) ||
			!(luaThreadId = (*env)->GetFieldID(env, luaStateClass, "luaThread", "J")) ||
			!(yieldId = (*env)->GetFieldID(env, luaStateClass, "yield", "Z"))) {
		return JNLUA_JNIVERSION;
	}
	if (!(javaFunctionInterface = referenceClass(env, "com/naef/jnlua/JavaFunction")) ||
			!(invokeId = (*env)->GetMethodID(env, javaFunctionInterface, "invoke", "(Lcom/naef/jnlua/LuaState;)I"))) {
		return JNLUA_JNIVERSION;
	}
	if (!(luaRuntimeExceptionClass = referenceClass(env, "com/naef/jnlua/LuaRuntimeException")) ||
			!(luaRuntimeExceptionInitId = (*env)->GetMethodID(env, luaRuntimeExceptionClass, "<init>", "(Ljava/lang/String;)V")) ||
			!(setLuaErrorId = (*env)->GetMethodID(env, luaRuntimeExceptionClass, "setLuaError", "(Lcom/naef/jnlua/LuaError;)V"))) {
		return JNLUA_JNIVERSION;
	}
	if (!(luaSyntaxExceptionClass = referenceClass(env, "com/naef/jnlua/LuaSyntaxException")) ||
			!(luaSyntaxExceptionInitId = (*env)->GetMethodID(env, luaSyntaxExceptionClass, "<init>", "(Ljava/lang/String;)V"))) {
		return JNLUA_JNIVERSION;
	}
	if (!(luaMemoryAllocationExceptionClass = referenceClass(env, "com/naef/jnlua/LuaMemoryAllocationException")) ||
			!(luaMemoryAllocationExceptionInitId = (*env)->GetMethodID(env, luaMemoryAllocationExceptionClass, "<init>", "(Ljava/lang/String;)V"))) {
		return JNLUA_JNIVERSION;
	}
	if (!(luaGcMetamethodExceptionClass = referenceClass(env, "com/naef/jnlua/LuaGcMetamethodException")) ||
			!(luaGcMetamethodExceptionInitId = (*env)->GetMethodID(env, luaGcMetamethodExceptionClass, "<init>", "(Ljava/lang/String;)V"))) {
		return JNLUA_JNIVERSION;
	}
	if (!(luaMessageHandlerExceptionClass = referenceClass(env, "com/naef/jnlua/LuaMessageHandlerException")) ||
			!(luaMessageHandlerExceptionInitId = (*env)->GetMethodID(env, luaMessageHandlerExceptionClass, "<init>", "(Ljava/lang/String;)V"))) {
		return JNLUA_JNIVERSION;
	}
	if (!(luaStackTraceElementClass = referenceClass(env, "com/naef/jnlua/LuaStackTraceElement")) ||
			!(luaStackTraceElementInitId = (*env)->GetMethodID(env, luaStackTraceElementClass, "<init>", "(Ljava/lang/String;Ljava/lang/String;I)V"))) {
		return JNLUA_JNIVERSION;
	}
	if (!(luaErrorClass = referenceClass(env, "com/naef/jnlua/LuaError")) ||
			!(luaErrorInitId = (*env)->GetMethodID(env, luaErrorClass, "<init>", "(Ljava/lang/String;Ljava/lang/Throwable;)V")) ||
			!(setLuaStackTraceId = (*env)->GetMethodID(env, luaErrorClass, "setLuaStackTrace", "([Lcom/naef/jnlua/LuaStackTraceElement;)V"))) {
		return JNLUA_JNIVERSION;
	}
	if (!(throwableClass = referenceClass(env, "java/lang/Throwable")) ||
			!(getMessageId = (*env)->GetMethodID(env, throwableClass, "getMessage", "()Ljava/lang/String;"))) {
		return JNLUA_JNIVERSION;
	}
	if (!(nullPointerExceptionClass = referenceClass(env, "java/lang/NullPointerException"))) {
		return JNLUA_JNIVERSION;
	}
	if (!(illegalArgumentExceptionClass = referenceClass(env, "java/lang/IllegalArgumentException"))) {
		return JNLUA_JNIVERSION;
	}
	if (!(illegalStateExceptionClass = referenceClass(env, "java/lang/IllegalStateException"))) {
		return JNLUA_JNIVERSION;
	}
	if (!(inputStreamClass = referenceClass(env, "java/io/InputStream")) ||
			!(readId = (*env)->GetMethodID(env, inputStreamClass, "read", "([B)I"))) {
		return JNLUA_JNIVERSION;
	}
	if (!(outputStreamClass = referenceClass(env, "java/io/OutputStream")) ||
			!(writeId = (*env)->GetMethodID(env, outputStreamClass, "write", "([BII)V"))) {
		return JNLUA_JNIVERSION;
	}
	if (!(ioExceptionClass = referenceClass(env, "java/io/IOException"))) {
		return JNLUA_JNIVERSION;
	}
	if (!(enumClass = referenceClass(env, "java/lang/Enum")) ||
			!(nameId = (*env)->GetMethodID(env, enumClass, "name", "()Ljava/lang/String;"))) {
		return JNLUA_JNIVERSION;
	}

	/* OK */
	initialized = 1;
	return JNLUA_JNIVERSION;
}

/* Handles the unloading of this library. */
JNIEXPORT void JNICALL JNI_OnUnload (JavaVM *vm, void *reserved) {
	JNIEnv *env;
	
	/* Get environment */
	if ((*vm)->GetEnv(vm, (void **) &env, JNLUA_JNIVERSION) != JNI_OK) {
		return;
	}
	
	/* Free classes */
	if (luaStateClass) {
		(*env)->DeleteGlobalRef(env, luaStateClass);
	}
	if (javaFunctionInterface) {
		(*env)->DeleteGlobalRef(env, javaFunctionInterface);
	}
	if (luaRuntimeExceptionClass) {
		(*env)->DeleteGlobalRef(env, luaRuntimeExceptionClass);
	}
	if (luaSyntaxExceptionClass) {
		(*env)->DeleteGlobalRef(env, luaSyntaxExceptionClass);
	}
	if (luaMemoryAllocationExceptionClass) {
		(*env)->DeleteGlobalRef(env, luaMemoryAllocationExceptionClass);
	}
	if (luaGcMetamethodExceptionClass) {
		(*env)->DeleteGlobalRef(env, luaGcMetamethodExceptionClass);
	}
	if (luaMessageHandlerExceptionClass) {
		(*env)->DeleteGlobalRef(env, luaMessageHandlerExceptionClass);
	}
	if (luaStackTraceElementClass) {
		(*env)->DeleteGlobalRef(env, luaStackTraceElementClass);
	}
	if (luaErrorClass) {
		(*env)->DeleteGlobalRef(env, luaErrorClass);
	}
	if (throwableClass) {
		(*env)->DeleteGlobalRef(env, throwableClass);
	}
	if (nullPointerExceptionClass) {
		(*env)->DeleteGlobalRef(env, nullPointerExceptionClass);
	}
	if (illegalArgumentExceptionClass) {
		(*env)->DeleteGlobalRef(env, illegalArgumentExceptionClass);
	}
	if (illegalStateExceptionClass) {
		(*env)->DeleteGlobalRef(env, illegalStateExceptionClass);
	}
	if (inputStreamClass) {
		(*env)->DeleteGlobalRef(env, inputStreamClass);
	}
	if (outputStreamClass) {
		(*env)->DeleteGlobalRef(env, outputStreamClass);
	}
	if (ioExceptionClass) {
		(*env)->DeleteGlobalRef(env, ioExceptionClass);
	}
	if (enumClass) {
		(*env)->DeleteGlobalRef(env, enumClass);
	}
}

/* ---- JNI helpers ---- */
/* Finds a class and returns a new JNI global reference to it. */
static jclass referenceClass (JNIEnv *env, const char *className) {
	jclass clazz;
	
	clazz = (*env)->FindClass(env, className);
	if (!clazz) {
		return NULL;
	}
	return (*env)->NewGlobalRef(env, clazz);
}

/* Returns a new JNI global reference. */
static jobject newGlobalRef (JNIEnv *env, lua_State *luaState, jobject obj, int type) {
	jobject ref;

	checknotnull(env, luaState, obj);
	if (type == JNLUA_HARDREF) {
		ref = (*env)->NewGlobalRef(env, obj);
		check(env, luaState, ref != NULL, luaMemoryAllocationExceptionClass, "JNI error: NewGlobalRef() failed");
	} else {
		ref = (*env)->NewWeakGlobalRef(env, obj);
		check(env, luaState, ref != NULL, luaMemoryAllocationExceptionClass, "JNI error: NewWeakGlobalRef() failed");
	}
	return ref;
}

/* Return a new JNI byte array. */
static jbyteArray newByteArray (JNIEnv *env, lua_State *luaState, jsize length) {
	jbyteArray array;
	
	array = (*env)->NewByteArray(env, length);
	check(env, luaState, array != NULL, luaMemoryAllocationExceptionClass, "JNI error: NewByteArray() failed");
	return array;
}

/* Returns the JNI UTF chars of a string. */
static const char *getStringUtfChars (JNIEnv *env, lua_State *luaState, jstring string) {
	const char *utf;

	checknotnull(env, luaState, string);
	utf = (*env)->GetStringUTFChars(env, string, NULL);
	check(env, luaState, utf != NULL, luaMemoryAllocationExceptionClass, "JNI error: getStringUTFChars() failed");
	return utf;
}

/* ---- Lua helpers ---- */
/* Checks stack space. */
static void checkstack (lua_State *luaState, int space, const char *msg) {
	if (!lua_checkstack(luaState, space)) {
		if (msg) {
			luaL_error(luaState, "stack overflow (%s)", msg);
		} else {
			luaL_error(luaState, "stack overflow");
		}
	}
}

/* ---- Java state operations ---- */
/* Returns the Lua state from the Java state. */
static lua_State *getLuaState (JNIEnv *env, jobject obj) {
	return (lua_State *) (uintptr_t) (*env)->GetLongField(env, obj, luaStateId);
}

/* Sets the Lua state in the Java state. */
static void setLuaState (JNIEnv *env, jobject obj, lua_State *luaState) {
	(*env)->SetLongField(env, obj, luaStateId, (jlong) (uintptr_t) luaState);
}

/* Returns the Lua thread from the Java state. */
static lua_State *getLuaThread (JNIEnv *env, jobject obj) {
	return (lua_State *) (uintptr_t) (*env)->GetLongField(env, obj, luaThreadId);
}

/* Sets the Lua state in the Java state. */
static void setLuaThread (JNIEnv *env, jobject obj, lua_State *luaState) {
	(*env)->SetLongField(env, obj, luaThreadId, (jlong) (uintptr_t) luaState);
}

/* Returns the yield flag from the Java state */
static jboolean getYield (JNIEnv *env, jobject obj) {
	return (*env)->GetBooleanField(env, obj, yieldId);
}

/* Sets the yield flag in the Java state */
static void setYield (JNIEnv *env, jobject obj, jboolean yield) {
	(*env)->SetBooleanField(env, obj, yieldId, yield);
}

/* ---- Lua state operations ---- */
/* Returns the JNI environment from the Lua state. */
static JNIEnv *getJniEnv (lua_State *luaState) {
	JNIEnv* env;
	
	lua_getfield(luaState, LUA_REGISTRYINDEX, JNLUA_RENV);
	env = (JNIEnv *) lua_touserdata(luaState, -1);
	lua_pop(luaState, 1);
	return env;
}

/* Sets the JNI environment in the Lua state. */
static void setJniEnv (lua_State* luaState, JNIEnv *env) {
	lua_pushlightuserdata(luaState, (void *) env);
	lua_setfield(luaState, LUA_REGISTRYINDEX, JNLUA_RENV);
}

/* Returns the Java state from the Lua state. */
static jobject getJavaState (lua_State *luaState) {
	jobject obj;
	
	lua_getfield(luaState, LUA_REGISTRYINDEX, JNLUA_RJAVASTATE);
	obj = (jobject) lua_touserdata(luaState, -1);
	lua_pop(luaState, 1);
	return obj;
}

/* Sets the Java state in the Lua state. */
static void setJavaState (lua_State *luaState, jobject javaState) {
	lua_pushlightuserdata(luaState, javaState);
	lua_setfield(luaState, LUA_REGISTRYINDEX, JNLUA_RJAVASTATE);
}

/* ---- Checks ---- */
/* Returns whether an index is valid. */
static int validindex (lua_State *luaState, int index) {
	int top;
	
	top = lua_gettop(luaState);
	if (index <= 0) {
		if (index > LUA_REGISTRYINDEX) {
			index = top + index + 1;
		} else {
			switch (index) {
			case LUA_REGISTRYINDEX:
				return 1;
			default:
				return 0; /* C upvalue access not needed, don't even validate */
			}
		}
	}
	return index >= 1 && index <= top;
}

/* Checks if an index is valid. */
static void checkindex (JNIEnv *env, lua_State *luaState, int index) {
	checkarg(env, luaState, validindex(luaState, index), "illegal index");
}
	
/* Checks if an index is valid, ignoring pseudo indexes. */
static void checkrealindex (JNIEnv *env, lua_State *luaState, int index) {
	int top;
	
	top = lua_gettop(luaState);
	if (index <= 0) {
		index = top + index + 1;
	}
	checkarg(env, luaState, index >= 1 && index <= top, "illegal index");
}

/* Checks the type of a stack value. */
static void checktype (JNIEnv *env, lua_State *luaState, int index, int type) {
	checkindex(env, luaState, index);
	checkarg(env, luaState, lua_type(luaState, index) == type, "illegal type");
}
	
/* Checks that there are at least n values on the stack. */
static void checknelems (JNIEnv *env, lua_State *luaState, int n) {
	checkstate(env, luaState, lua_gettop(luaState) >= n, "stack underflow");
}

/* Checks an argument for not-null. */ 
static void checknotnull (JNIEnv *env, lua_State *luaState, void *object) {
	check(env, luaState, object != NULL, nullPointerExceptionClass, "null");
}

/* Checks an argument condition. */
static void checkarg (JNIEnv *env, lua_State *luaState, int cond, const char *msg) {
	check(env, luaState, cond, illegalArgumentExceptionClass, msg);
}

/* Checks a state condition. */
static void checkstate (JNIEnv *env, lua_State *luaState, int cond, const char *msg) {
	check(env, luaState, cond, illegalStateExceptionClass, msg);
}

/* Checks a condition. */
static void check (JNIEnv *env, lua_State *luaState, int cond, jthrowable throwableClass, const char *msg) {
	if (!cond) {
		throw(env, luaState, throwableClass, msg);
	}
}

/* Throws an exception */
static void throw (JNIEnv *env, lua_State *luaState, jthrowable throwableClass, const char *msg) {
	(*env)->ThrowNew(env, throwableClass, msg);
	longjmp(luaState->errorJmp->b, -1);
}

/* ---- Java object helpers ---- */
/* Pushes a Java object on the stack. */
static void pushJavaObject (JNIEnv *env, lua_State *luaState, jobject object) {
	jobject *userData;
	
	userData = (jobject *) lua_newuserdata(luaState, sizeof(jobject));
	luaL_getmetatable(luaState, JNLUA_MOBJECT);
	*userData = newGlobalRef(env, luaState, object, JNLUA_HARDREF);
	lua_setmetatable(luaState, -2);
}
	
/* Returns the Java object at the specified index, or NULL if such an object is unobtainable. */
static jobject getJavaObject (JNIEnv *env, lua_State *luaState, int index, jclass class) {
	int result;
	jobject object;

	if (!lua_isuserdata(luaState, index)) {
		return NULL;
	}
	if (!lua_getmetatable(luaState, index)) {
		return NULL;
	}
	luaL_getmetatable(luaState, JNLUA_MOBJECT);
	result = lua_rawequal(luaState, -1, -2);
	lua_pop(luaState, 2);
	if (!result) {
		return NULL;
	}
	object = *(jobject *) lua_touserdata(luaState, index);
	if (class) {
		if (!(*env)->IsInstanceOf(env, object, class)) {
			return NULL;
		}
	}
	return object;
}

/* Returns a Java string for a value on the stack. */
static jstring toString (JNIEnv *env, lua_State *luaState, int index) {
	jstring string;

	string = (*env)->NewStringUTF(env, luaL_tolstring(luaState, index, NULL));
	lua_pop(luaState, 1);
	return string;
}

/* ---- Metamethods ---- */
/* Finalizes Java objects. */
static int gcJavaObject (lua_State *luaState) {
	JNIEnv* env;
	jobject obj;
	
	env = getJniEnv(luaState);
	if (!env) {
		/* Java VM has been destroyed. Nothing to do. */
		return 0;
	}
	obj = *(jobject *) lua_touserdata(luaState, 1);
	(*env)->DeleteGlobalRef(env, obj);
	return 0;
}

/* Calls a Java function. If an exception is reported, store it as the cause for later use. */
static int callJavaFunction (lua_State *luaState) {
	JNIEnv* env;
	jobject obj;
	jobject javaFunctionObj;
	lua_State *javaLuaThread;
	int result;
	jthrowable throwable;
	jstring whereString;
	
	/* Get Java context. */
	env = getJniEnv(luaState);
	obj = getJavaState(luaState);
	if (!obj) {
		lua_pushliteral(luaState, "no Java VM");
		return lua_error(luaState);
	}
	
	/* Get Java function object. */
	lua_pushvalue(luaState, lua_upvalueindex(1));
	javaFunctionObj = getJavaObject(env, luaState, -1, javaFunctionInterface);
	lua_pop(luaState, 1);
	if (!javaFunctionObj) {
		/* Function was cleared from outside JNLua code. */
		lua_pushliteral(luaState, "no Java function");
		return lua_error(luaState);
	}
	
	/* Perform the call, handling coroutine situations. */
	setYield(env, obj, JNI_FALSE);
	javaLuaThread = getLuaThread(env, obj);
	if (javaLuaThread == luaState) {
		result = (*env)->CallIntMethod(env, javaFunctionObj, invokeId, obj);
	} else {
		setLuaThread(env, obj, luaState);
		result = (*env)->CallIntMethod(env, javaFunctionObj, invokeId, obj);
		setLuaThread(env, obj, javaLuaThread);
	}
	
	/* Handle exception */
	throwable = (*env)->ExceptionOccurred(env);
	if (throwable) {
		/* Push exception & clear */
		lua_settop(luaState, 0);
		luaL_where(luaState, 1);
		whereString = toString(env, luaState, -1);
		lua_pop(luaState, 1);
		pushJavaObject(env, luaState, (*env)->NewObject(env, luaErrorClass,
				luaErrorInitId, whereString, throwable));
		(*env)->ExceptionClear(env);
		
		/* Error out */
		return lua_error(luaState);
	}
	
	/* Handle yield */
	if (getYield(env, obj)) {
		if (result < 0 || result > lua_gettop(luaState)) {
			lua_pushliteral(luaState, "illegal return count");
			return lua_error(luaState);
		}
		return lua_yield(luaState, result);
	}
	
	return result;
}

/* ---- Error handling ---- */
/* Handles Lua errors. */
static int handleError (lua_State *luaState) {
	JNIEnv *env;
	jstring messageString;
	int level;
	int count;
	lua_Debug ar;
	jobjectArray luaStackTraceArray;
	jstring functionNameString;
	jstring sourceNameString;
	jobject luaStackTraceElementObj;
	jobject luaErrorObj;

	/* Get the JNI environment. */
	env = getJniEnv(luaState);

	/* Count relevant stack frames */
	level = 1;
	count = 0;
	while (lua_getstack(luaState, level, &ar)) {
		lua_getinfo(luaState, "nSl", &ar);
		if (processActivationRecord(&ar)) {
			count++;
		}
		level++;
	}
	
	/* Create Lua stack trace as a Java LuaStackTraceElement[] */
	luaStackTraceArray = (*env)->NewObjectArray(env, count, luaStackTraceElementClass, NULL);
	if (!luaStackTraceArray) {
		return 1;
	}
	level = 1;
	count = 0;
	while (lua_getstack(luaState, level, &ar)) {
		lua_getinfo(luaState, "nSl", &ar);
		if (processActivationRecord(&ar)) {
			if (ar.name) {
				functionNameString = (*env)->NewStringUTF(env, ar.name);
			} else {
				functionNameString = NULL;
			}
			if (ar.source) {
				sourceNameString = (*env)->NewStringUTF(env, ar.source);
			} else {
				sourceNameString = NULL;
			}
			luaStackTraceElementObj = (*env)->NewObject(env, luaStackTraceElementClass,
					luaStackTraceElementInitId, functionNameString, sourceNameString, ar.currentline);
			if (!luaStackTraceElementObj) {
				return 1;
			}
			(*env)->SetObjectArrayElement(env, luaStackTraceArray, count, luaStackTraceElementObj);
			if ((*env)->ExceptionCheck(env)) {
				return 1;
			}
			count++;
		}
		level++;
	}
	
	/* Get or create the error object  */
	luaErrorObj = getJavaObject(env, luaState, -1, luaErrorClass);
	if (!luaErrorObj) {
		messageString = toString(env, luaState, -1);
		if (!(luaErrorObj = (*env)->NewObject(env, luaErrorClass, luaErrorInitId, messageString, NULL))) {
			return 1;
		}
	}
	(*env)->CallVoidMethod(env, luaErrorObj, setLuaStackTraceId, luaStackTraceArray);
	
	/* Replace error */
	pushJavaObject(env, luaState, luaErrorObj);
	return 1;
}

/* Processes a Lua activation record and returns whether it is relevant. */
static int processActivationRecord (lua_Debug *ar) {
	if (ar->name && strlen(ar->name) == 0) {
		ar->name = NULL;
	}
	if (ar->what && strcmp(ar->what, "C") == 0) {
		ar->source = NULL;
	}
	if (ar->source) {
		if (*ar->source == '=' || *ar->source == '@') {
			ar->source++;
		}
	}
	return ar->name || ar->source;
}

/* Handles Lua errors by throwing a Java exception. */
static void throwException (JNIEnv* env, lua_State *luaState, int status) {
	jclass throwableClass;
	jmethodID throwableInitId;
	jthrowable throwable;
	jobject luaErrorObj;
	
	/* Determine the type of exception to throw. */
	switch (status) {
	case LUA_ERRSYNTAX:
		throwableClass = luaSyntaxExceptionClass;
		throwableInitId = luaSyntaxExceptionInitId;
		break;
	case LUA_ERRMEM:
		throwableClass = luaMemoryAllocationExceptionClass;
		throwableInitId = luaMemoryAllocationExceptionInitId;
		break;
	case LUA_ERRERR:
		throwableClass = luaMessageHandlerExceptionClass;
		throwableInitId = luaMessageHandlerExceptionInitId;
		break;
	case LUA_ERRGCMM:
		throwableClass = luaGcMetamethodExceptionClass;
		throwableInitId = luaGcMetamethodExceptionInitId;
		break;
	default:
		throwableClass = luaRuntimeExceptionClass;
		throwableInitId = luaRuntimeExceptionInitId;
	}

	/* Create exception */
	throwable = (*env)->NewObject(env, throwableClass, throwableInitId, toString(env, luaState, -1));
	if (!throwable) {
		return;
	}
	
	/* Set the Lua error, if any. */
	luaErrorObj = getJavaObject(env, luaState, -1, luaErrorClass);
	if (luaErrorObj && throwableClass == luaRuntimeExceptionClass) {
		(*env)->CallVoidMethod(env, throwable, setLuaErrorId, luaErrorObj);
	}
	
	/* Throw */
	if ((*env)->Throw(env, throwable) < 0) {
		return;
	}
	
	/* Pop error */
	lua_pop(luaState, 1);
}

/* ---- Stream adapters ---- */
/* Lua reader for Java input streams. */
static const char *readInputStream (lua_State *luaState, void *ud, size_t *size) {
	Stream *stream;
	int read;

	stream = (Stream *) ud;
	read = (*stream->env)->CallIntMethod(stream->env, stream->stream, readId, stream->byteArray);
	if ((*stream->env)->ExceptionCheck(stream->env)) {
		return NULL;
	}
	if (read == -1) {
		return NULL;
	}
	if (stream->bytes && stream->isCopy) {
		(*stream->env)->ReleaseByteArrayElements(stream->env, stream->byteArray, stream->bytes, JNI_ABORT);
		stream->bytes = NULL;
	}
	if (!stream->bytes) {
		stream->bytes = (*stream->env)->GetByteArrayElements(stream->env, stream->byteArray, &stream->isCopy);
		if (!stream->bytes) {
			(*stream->env)->ThrowNew(stream->env, ioExceptionClass, "error accessing IO buffer");
			return NULL;
		}
	}
	*size = read;
	return (const char *) stream->bytes;
}

/* Lua writer for Java output streams. */
static int writeOutputStream (lua_State *luaState, const void *data, size_t size, void *ud) {
	Stream *stream;

	stream = (Stream *) ud;
	if (!stream->bytes) {
		stream->bytes = (*stream->env)->GetByteArrayElements(stream->env, stream->byteArray, &stream->isCopy);
		if (!stream->bytes) {
			(*stream->env)->ThrowNew(stream->env, ioExceptionClass, "error accessing IO buffer");
			return 1;
		}
	}
	memcpy(stream->bytes, data, size);
	if (stream->isCopy) {
		(*stream->env)->ReleaseByteArrayElements(stream->env, stream->byteArray, stream->bytes, JNI_COMMIT);
	}
	(*stream->env)->CallVoidMethod(stream->env, stream->stream, writeId, stream->byteArray, 0, size);
	if ((*stream->env)->ExceptionCheck(stream->env)) {
		return 1;
	}
	return 0;
}
