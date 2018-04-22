#include <jni.h>
#include <string>
#include <assert.h>
#include <memory.h>
#include <opnmidi.h>

#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

pthread_mutex_t g_lock;
bool mutex_created = false;

typedef int (*AndroidAudioCallback)(short *buffer, int num_samples);
bool OpenSLWrap_Init(AndroidAudioCallback cb);
void OpenSLWrap_Shutdown();

#if 1
#undef JNIEXPORT
#undef JNICALL
#define JNIEXPORT extern "C"
#define JNICALL
#endif

/************************************************************************************************
 * Minimal OpenSL ES wrapper implementation got from https://gist.github.com/hrydgard/3072540
 ************************************************************************************************/
/*
 * Was been modified:
 * - Added support of dynamic size per buffer chunk (allow callback return custom buffer size)
 * - Added posix mutexes to prevent rases on play/stop
 * - First chunk keep zeroed to don't take choppy glitch on begin
 */

// This is kinda ugly, but for simplicity I've left these as globals just like in the sample,
// as there's not really any use case for this where we have multiple audio devices yet.
// engine interfaces
static SLObjectItf  engineObject;
static SLEngineItf  engineEngine;
static SLObjectItf  outputMixObject;

// buffer queue player interfaces
static SLObjectItf  bqPlayerObject = NULL;
static SLPlayItf    bqPlayerPlay;
static SLAndroidSimpleBufferQueueItf    bqPlayerBufferQueue;
static SLMuteSoloItf                    bqPlayerMuteSolo;
static SLVolumeItf                      bqPlayerVolume;
#define BUFFER_SIZE 40960
#define BUFFER_SIZE_IN_SAMPLES (BUFFER_SIZE / 2)

// Double buffering.
static int      bufferLen[2] = {0, 0};
static short    buffer[2][BUFFER_SIZE_IN_SAMPLES];
static int      curBuffer = 0;
static AndroidAudioCallback audioCallback;

OPN2_MIDIPlayer* playingDevice = NULL;

// This callback handler is called every time a buffer finishes playing.
// The documentation available is very unclear about how to best manage buffers.
// I've chosen to this approach: Instantly enqueue a buffer that was rendered to the last time,
// and then render the next. Hopefully it's okay to spend time in this callback after having enqueued.
static void bqPlayerCallback(SLAndroidSimpleBufferQueueItf bq, void *context)
{
    assert(bq == bqPlayerBufferQueue);
    assert(NULL == context);
    pthread_mutex_lock(&g_lock);
    short *nextBuffer = buffer[curBuffer];
    int nextSize = bufferLen[curBuffer];
    if(nextSize > 0)
    {
        SLresult result;
        result = (*bqPlayerBufferQueue)->Enqueue(bqPlayerBufferQueue, nextBuffer, nextSize*2);
        // Comment from sample code:
        // the most likely other result is SL_RESULT_BUFFER_INSUFFICIENT,
        // which for this code example would indicate a programming error
        assert(SL_RESULT_SUCCESS == result);
        curBuffer ^= 1;  // Switch buffer
    }
    // Render to the fresh buffer
    bufferLen[curBuffer] = audioCallback(buffer[curBuffer], BUFFER_SIZE_IN_SAMPLES);
    pthread_mutex_unlock(&g_lock);
}

// create the engine and output mix objects
bool OpenSLWrap_Init(AndroidAudioCallback cb)
{
    audioCallback = cb;
    SLresult result;

    memset(buffer, 0, BUFFER_SIZE*2);
    bufferLen[0] = BUFFER_SIZE_IN_SAMPLES;
    bufferLen[1] = BUFFER_SIZE_IN_SAMPLES;

    // create engine
    result = slCreateEngine(&engineObject, 0, NULL, 0, NULL, NULL);
    assert(SL_RESULT_SUCCESS == result);
    result = (*engineObject)->Realize(engineObject, SL_BOOLEAN_FALSE);
    assert(SL_RESULT_SUCCESS == result);
    result = (*engineObject)->GetInterface(engineObject, SL_IID_ENGINE, &engineEngine);
    assert(SL_RESULT_SUCCESS == result);
    result = (*engineEngine)->CreateOutputMix(engineEngine, &outputMixObject, 0, 0, 0);
    assert(SL_RESULT_SUCCESS == result);
    result = (*outputMixObject)->Realize(outputMixObject, SL_BOOLEAN_FALSE);
    assert(SL_RESULT_SUCCESS == result);

    SLDataLocator_AndroidSimpleBufferQueue loc_bufq = {SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 2};
    SLDataFormat_PCM format_pcm = {
            SL_DATAFORMAT_PCM,
            2,
            SL_SAMPLINGRATE_44_1,
            SL_PCMSAMPLEFORMAT_FIXED_16,
            SL_PCMSAMPLEFORMAT_FIXED_16,
            SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT,
            SL_BYTEORDER_LITTLEENDIAN
    };
    SLDataSource audioSrc = {&loc_bufq, &format_pcm};

    // configure audio sink
    SLDataLocator_OutputMix loc_outmix = {SL_DATALOCATOR_OUTPUTMIX, outputMixObject};
    SLDataSink audioSnk = {&loc_outmix, NULL};

    // create audio player
    const SLInterfaceID ids[2] = {SL_IID_BUFFERQUEUE, SL_IID_VOLUME};
    const SLboolean req[2] = {SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE};
    result = (*engineEngine)->CreateAudioPlayer(engineEngine, &bqPlayerObject, &audioSrc, &audioSnk, 2, ids, req);
    assert(SL_RESULT_SUCCESS == result);
    result = (*bqPlayerObject)->Realize(bqPlayerObject, SL_BOOLEAN_FALSE);
    assert(SL_RESULT_SUCCESS == result);
    result = (*bqPlayerObject)->GetInterface(bqPlayerObject, SL_IID_PLAY, &bqPlayerPlay);
    assert(SL_RESULT_SUCCESS == result);
    result = (*bqPlayerObject)->GetInterface(bqPlayerObject, SL_IID_BUFFERQUEUE,
                                             &bqPlayerBufferQueue);
    assert(SL_RESULT_SUCCESS == result);
    result = (*bqPlayerBufferQueue)->RegisterCallback(bqPlayerBufferQueue, bqPlayerCallback, NULL);
    assert(SL_RESULT_SUCCESS == result);
    result = (*bqPlayerObject)->GetInterface(bqPlayerObject, SL_IID_VOLUME, &bqPlayerVolume);
    assert(SL_RESULT_SUCCESS == result);
    result = (*bqPlayerPlay)->SetPlayState(bqPlayerPlay, SL_PLAYSTATE_PLAYING);
    assert(SL_RESULT_SUCCESS == result);

    // Render and enqueue a first buffer. (or should we just play the buffer empty?)
    curBuffer = 0;//Just pass silence (this frame is always produces chopping, but next are fine)
    //bufferLen[curBuffer] = audioCallback(buffer[curBuffer], BUFFER_SIZE_IN_SAMPLES);
    result = (*bqPlayerBufferQueue)->Enqueue(bqPlayerBufferQueue, buffer[curBuffer], sizeof(buffer[curBuffer]));
    if (SL_RESULT_SUCCESS != result) {
        return false;
    }
    curBuffer ^= 1;
    return true;
}

//shut down the native audio system
void OpenSLWrap_Shutdown()
{
    pthread_mutex_lock(&g_lock);
    SLresult result;
    result = (*bqPlayerPlay)->SetPlayState(bqPlayerPlay, SL_PLAYSTATE_STOPPED);
    assert(SL_RESULT_SUCCESS == result);

    memset(buffer, 0, BUFFER_SIZE*2);
    bufferLen[0] = BUFFER_SIZE_IN_SAMPLES;
    bufferLen[1] = BUFFER_SIZE_IN_SAMPLES;

    if (bqPlayerObject != NULL) {
        (*bqPlayerObject)->Destroy(bqPlayerObject);
        bqPlayerObject = NULL;
        bqPlayerPlay = NULL;
        bqPlayerBufferQueue = NULL;
        bqPlayerMuteSolo = NULL;
        bqPlayerVolume = NULL;
    }
    if (outputMixObject != NULL) {
        (*outputMixObject)->Destroy(outputMixObject);
        outputMixObject = NULL;
    }
    if (engineObject != NULL) {
        (*engineObject)->Destroy(engineObject);
        engineObject = NULL;
        engineEngine = NULL;
    }
    pthread_mutex_unlock(&g_lock);
}

/************************************************************************************************
 ********************** Minimal OpenSL ES wrapper implementation END ****************************
 ************************************************************************************************/

int audioCallbackFunction(short *buffer, int num_samples)
{
    return opn2_play(playingDevice, num_samples, buffer);
}

void infiniteLoopStream(OPN2_MIDIPlayer* device)
{
    if(mutex_created) {
        assert(pthread_mutex_init(&g_lock, NULL) == 0);
        mutex_created=true;
    }
    playingDevice = device;
    OpenSLWrap_Init(audioCallbackFunction);
}


#define ADLDEV (OPN2_MIDIPlayer*)device

JNIEXPORT void JNICALL
Java_ru_wohlsoft_opnmidiplayer_Player_startPlaying(JNIEnv *env, jobject instance, jlong device)
{
    pthread_mutex_lock(&g_lock);
    infiniteLoopStream(ADLDEV);
    pthread_mutex_unlock(&g_lock);
}

JNIEXPORT void JNICALL
Java_ru_wohlsoft_opnmidiplayer_Player_stopPlaying(JNIEnv *env, jobject instance)
{
    OpenSLWrap_Shutdown();
}


JNIEXPORT jstring JNICALL
Java_ru_wohlsoft_opnmidiplayer_Player_adl_1errorString(JNIEnv *env, jobject instance)
{
    pthread_mutex_lock(&g_lock);
    const char* adlMIDIerr = opn2_errorString();
    jstring ret = env->NewStringUTF(adlMIDIerr);
    pthread_mutex_unlock(&g_lock);
    return ret;
}

JNIEXPORT jstring JNICALL
Java_ru_wohlsoft_opnmidiplayer_Player_adl_1errorInfo(JNIEnv *env, jobject instance, jlong device) {

    pthread_mutex_lock(&g_lock);
    const char* adlMIDIerr = opn2_errorInfo(ADLDEV);
    jstring ret = env->NewStringUTF(adlMIDIerr);
    pthread_mutex_unlock(&g_lock);
    return ret;
}

JNIEXPORT jstring JNICALL
Java_ru_wohlsoft_opnmidiplayer_Player_stringFromJNI(JNIEnv *env, jobject /* this */)
{
    std::string hello = "OPN2 Emulator is ready";
    return env->NewStringUTF(hello.c_str());
}

JNIEXPORT jint JNICALL
Java_ru_wohlsoft_opnmidiplayer_Player_adl_1setNumCards(JNIEnv *env, jobject instance, jlong device,
                                                       jint numCards) {
    pthread_mutex_lock(&g_lock);
    jint ret = (jint)opn2_setNumChips(ADLDEV, (int)numCards);
    pthread_mutex_unlock(&g_lock);
    return ret;
}

JNIEXPORT jlong JNICALL
Java_ru_wohlsoft_opnmidiplayer_Player_adl_1init(JNIEnv *env, jobject instance, jlong sampleRate)
{
    return (jlong)opn2_init((long)sampleRate);
}

JNIEXPORT void JNICALL
Java_ru_wohlsoft_opnmidiplayer_Player_adl_1setScaleModulators(JNIEnv *env, jobject instance,
                                                              jlong device, jint smod)
{
    pthread_mutex_lock(&g_lock);
    opn2_setScaleModulators(ADLDEV, (int)smod);
    pthread_mutex_unlock(&g_lock);
}

JNIEXPORT void JNICALL
Java_ru_wohlsoft_opnmidiplayer_Player_adl_1setFullRangeBrightness(JNIEnv *env, jobject instance,
                                                              jlong device, jint fr_brightness)
{
    pthread_mutex_lock(&g_lock);
    opn2_setFullRangeBrightness(ADLDEV, (int)fr_brightness);
    pthread_mutex_unlock(&g_lock);
}


JNIEXPORT void JNICALL
Java_ru_wohlsoft_opnmidiplayer_Player_adl_1setLoopEnabled(JNIEnv *env, jobject instance,
                                                          jlong device, jint loopEn)
{
    pthread_mutex_lock(&g_lock);
    opn2_setLoopEnabled(ADLDEV, (int)loopEn);
    pthread_mutex_unlock(&g_lock);
}


JNIEXPORT jint JNICALL
Java_ru_wohlsoft_opnmidiplayer_Player_adl_1openBankFile(JNIEnv *env, jobject instance, jlong device,
                                                    jstring file_) {
    pthread_mutex_lock(&g_lock);
    const char *file = env->GetStringUTFChars(file_, 0);
    int ret = opn2_openBankFile(ADLDEV, (char*)file);
    env->ReleaseStringUTFChars(file_, file);
    pthread_mutex_unlock(&g_lock);
    return ret;
}

JNIEXPORT jint JNICALL
Java_ru_wohlsoft_opnmidiplayer_Player_adl_1openBankData(JNIEnv *env, jobject instance, jlong device,
                                                    jbyteArray array_) {
    pthread_mutex_lock(&g_lock);
    jbyte *array = env->GetByteArrayElements(array_, NULL);
    jsize length =  env->GetArrayLength(array_);
    jint ret = opn2_openBankData(ADLDEV, array, length);
    env->ReleaseByteArrayElements(array_, array, 0);
    pthread_mutex_unlock(&g_lock);
    return ret;
}


JNIEXPORT jint JNICALL
Java_ru_wohlsoft_opnmidiplayer_Player_adl_1openFile(JNIEnv *env, jobject instance, jlong device,
                                                    jstring file_) {
    pthread_mutex_lock(&g_lock);
    const char *file = env->GetStringUTFChars(file_, 0);
    int ret = opn2_openFile(ADLDEV, (char*)file);
    env->ReleaseStringUTFChars(file_, file);
    pthread_mutex_unlock(&g_lock);
    return ret;
}

JNIEXPORT jint JNICALL
Java_ru_wohlsoft_opnmidiplayer_Player_adl_1openData(JNIEnv *env, jobject instance, jlong device,
                                                    jbyteArray array_) {
    pthread_mutex_lock(&g_lock);
    jbyte *array = env->GetByteArrayElements(array_, NULL);
    jsize length =  env->GetArrayLength(array_);
    jint ret = opn2_openData(ADLDEV, array, length);
    env->ReleaseByteArrayElements(array_, array, 0);
    pthread_mutex_unlock(&g_lock);
    return ret;
}

JNIEXPORT void JNICALL
Java_ru_wohlsoft_opnmidiplayer_Player_adl_1reset(JNIEnv *env, jobject instance, jlong device) {
    pthread_mutex_lock(&g_lock);
    opn2_reset(ADLDEV);
    pthread_mutex_unlock(&g_lock);
}

JNIEXPORT void JNICALL
Java_ru_wohlsoft_opnmidiplayer_Player_adl_1close(JNIEnv *env, jobject instance, jlong device)
{
    pthread_mutex_lock(&g_lock);
    opn2_close(ADLDEV);
    pthread_mutex_unlock(&g_lock);
}

JNIEXPORT jint JNICALL
Java_ru_wohlsoft_opnmidiplayer_Player_adl_1play(JNIEnv *env, jobject instance, jlong device,
                                                jshortArray buffer_)
{
    pthread_mutex_lock(&g_lock);
    jshort *buffer = env->GetShortArrayElements(buffer_, NULL);
    jsize  length =  env->GetArrayLength(buffer_);
    short* outBuff = (short*)buffer;
    jint gotSamples = opn2_play(ADLDEV, length, outBuff);
    env->ReleaseShortArrayElements(buffer_, buffer, 0);
    pthread_mutex_unlock(&g_lock);
    return gotSamples;
}


JNIEXPORT jdouble JNICALL
Java_ru_wohlsoft_opnmidiplayer_Player_adl_1positionTell(JNIEnv *env, jobject instance, jlong device)
{
    pthread_mutex_lock(&g_lock);
    jdouble ret = (jdouble)opn2_positionTell(ADLDEV);
    pthread_mutex_unlock(&g_lock);
    return ret;
}

JNIEXPORT jdouble JNICALL
Java_ru_wohlsoft_opnmidiplayer_Player_adl_1totalTimeLength(JNIEnv *env, jobject instance, jlong device)
{
    pthread_mutex_lock(&g_lock);
    jdouble ret = (jdouble)opn2_totalTimeLength(ADLDEV);
    pthread_mutex_unlock(&g_lock);
    return ret;
}

JNIEXPORT void JNICALL
Java_ru_wohlsoft_opnmidiplayer_Player_adl_1positionSeek(JNIEnv *env, jobject instance, jlong device,
                                                        jdouble seconds)
{
    pthread_mutex_lock(&g_lock);
    opn2_positionSeek(ADLDEV, (double)seconds);
    pthread_mutex_unlock(&g_lock);
}


JNIEXPORT jstring JNICALL
Java_ru_wohlsoft_opnmidiplayer_Player_adl_1getBankName(JNIEnv *env, jobject instance, jint bank)
{
    std::string hello = "NoBanks";
    return env->NewStringUTF(hello.c_str());
}

JNIEXPORT void JNICALL
Java_ru_wohlsoft_opnmidiplayer_Player_adl_1setLogarithmicVolumes(JNIEnv *env, jobject instance,
                                                                 jlong device, jint logvol)
{
    pthread_mutex_lock(&g_lock);
    opn2_setLogarithmicVolumes(ADLDEV, (int)logvol);
    pthread_mutex_unlock(&g_lock);
}

JNIEXPORT void JNICALL
Java_ru_wohlsoft_opnmidiplayer_Player_adl_1setVolumeRangeModel(JNIEnv *env, jobject instance,
                                                               jlong device, jint volumeModel) {
    pthread_mutex_lock(&g_lock);
    opn2_setVolumeRangeModel(ADLDEV, (int)volumeModel);
    pthread_mutex_unlock(&g_lock);
}
