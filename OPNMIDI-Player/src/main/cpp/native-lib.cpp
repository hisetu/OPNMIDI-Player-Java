#include <jni.h>
#include <pthread.h>
#include <cassert>
#include <memory.h>
#include <opnmidi.h>
#include <string>

#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

#include <sys/system_properties.h>

pthread_mutex_t g_lock;
bool mutex_created = false;

typedef int8_t sample_t;

static OPNMIDI_AudioFormat g_audioFormat
{
    OPNMIDI_SampleType_S16,
    sizeof(int16_t),
    sizeof(int16_t) * 2
};

typedef int (*AndroidAudioCallback)(sample_t *buffer, int num_samples);
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
static SLObjectItf  bqPlayerObject = nullptr;
static SLPlayItf    bqPlayerPlay;
static SLAndroidSimpleBufferQueueItf    bqPlayerBufferQueue;
static SLMuteSoloItf                    bqPlayerMuteSolo;
static SLVolumeItf                      bqPlayerVolume;
#define BUFFER_SIZE 20480
#define BUFFER_SIZE_IN_SAMPLES (BUFFER_SIZE / 2)

// Double buffering.
static int      bufferLen[2] = {0, 0};
static sample_t buffer[2][BUFFER_SIZE * 4];
static size_t   curBuffer = 0;
static AndroidAudioCallback audioCallback;

static double   g_gaining = 2.0;

OPN2_MIDIPlayer* playingDevice = nullptr;

// This callback handler is called every time a buffer finishes playing.
// The documentation available is very unclear about how to best manage buffers.
// I've chosen to this approach: Instantly enqueue a buffer that was rendered to the last time,
// and then render the next. Hopefully it's okay to spend time in this callback after having enqueued.
static void bqPlayerCallback(SLAndroidSimpleBufferQueueItf bq, void *context)
{
    assert(bq == bqPlayerBufferQueue);
    assert(nullptr == context);
    pthread_mutex_lock(&g_lock);
    sample_t *nextBuffer = buffer[curBuffer];
    int nextSize = bufferLen[curBuffer];
    if(nextSize > 0)
    {
        SLresult result;
        result = (*bqPlayerBufferQueue)->Enqueue(bqPlayerBufferQueue, nextBuffer,
                                                 static_cast<SLuint32>(nextSize * g_audioFormat.containerSize));
        // Comment from sample code:
        // the most likely other result is SL_RESULT_BUFFER_INSUFFICIENT,
        // which for this code example would indicate a programming error
        assert(SL_RESULT_SUCCESS == result);
        curBuffer ^= 1u;  // Switch buffer
    }
    // Render to the fresh buffer
    bufferLen[curBuffer] = audioCallback(buffer[curBuffer], BUFFER_SIZE_IN_SAMPLES);
    pthread_mutex_unlock(&g_lock);
}

// create the engine and output mix objects
bool OpenSLWrap_Init(AndroidAudioCallback cb)
{
    int sdk_ver = 0;
    char sdk_ver_str[92] = {'\0'};
    if(__system_property_get("ro.build.version.sdk", sdk_ver_str))
    {
        sdk_ver = atoi(sdk_ver_str);
    }

    if(sdk_ver >= 21)
    {
        g_audioFormat.type = OPNMIDI_SampleType_F32;
        g_audioFormat.containerSize = sizeof(float);
        g_audioFormat.sampleOffset = sizeof(float) * 2;
    }

    audioCallback = cb;
    SLresult result;

    memset(buffer, 0, BUFFER_SIZE * g_audioFormat.sampleOffset);
    bufferLen[0] = BUFFER_SIZE_IN_SAMPLES;
    bufferLen[1] = BUFFER_SIZE_IN_SAMPLES;

    // create engine
    result = slCreateEngine(&engineObject, 0, nullptr, 0, nullptr, nullptr);
    assert(SL_RESULT_SUCCESS == result);
    result = (*engineObject)->Realize(engineObject, SL_BOOLEAN_FALSE);
    assert(SL_RESULT_SUCCESS == result);
    result = (*engineObject)->GetInterface(engineObject, SL_IID_ENGINE, &engineEngine);
    assert(SL_RESULT_SUCCESS == result);
    result = (*engineEngine)->CreateOutputMix(engineEngine, &outputMixObject, 0, nullptr, nullptr);
    assert(SL_RESULT_SUCCESS == result);
    result = (*outputMixObject)->Realize(outputMixObject, SL_BOOLEAN_FALSE);
    assert(SL_RESULT_SUCCESS == result);

    SLDataLocator_AndroidSimpleBufferQueue loc_bufq = {SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 2};

    SLAndroidDataFormat_PCM_EX format_pcm_ex;
    SLDataFormat_PCM format_pcm;
    SLDataSource audioSrc;

    if(sdk_ver >= 21)
    {
        /* for Android 21+*/
        format_pcm_ex.formatType = SL_ANDROID_DATAFORMAT_PCM_EX;
        format_pcm_ex.numChannels = 2;
        format_pcm_ex.sampleRate = SL_SAMPLINGRATE_44_1;
        format_pcm_ex.bitsPerSample = SL_PCMSAMPLEFORMAT_FIXED_32;
        format_pcm_ex.containerSize = SL_PCMSAMPLEFORMAT_FIXED_32;
        format_pcm_ex.channelMask = SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT;
        format_pcm_ex.endianness = SL_BYTEORDER_LITTLEENDIAN;
        format_pcm_ex.representation = SL_ANDROID_PCM_REPRESENTATION_FLOAT;
        audioSrc = {&loc_bufq, &format_pcm_ex};
    }
    else
    {
        /* for Android <21 */
        format_pcm.formatType = SL_DATAFORMAT_PCM;
        format_pcm.numChannels = 2;
        format_pcm.samplesPerSec = SL_SAMPLINGRATE_44_1;
        format_pcm.bitsPerSample = SL_PCMSAMPLEFORMAT_FIXED_16;
        format_pcm.containerSize = SL_PCMSAMPLEFORMAT_FIXED_16;
        format_pcm.channelMask = SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT;
        format_pcm.endianness = SL_BYTEORDER_LITTLEENDIAN;
        audioSrc = {&loc_bufq, &format_pcm};
    }

    // configure audio sink
    SLDataLocator_OutputMix loc_outmix = {SL_DATALOCATOR_OUTPUTMIX, outputMixObject};
    SLDataSink audioSnk = {&loc_outmix, nullptr};

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
    result = (*bqPlayerBufferQueue)->RegisterCallback(bqPlayerBufferQueue, bqPlayerCallback, nullptr);
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
    curBuffer ^= 1u;
    return true;
}

//shut down the native audio system
void OpenSLWrap_Shutdown()
{
    pthread_mutex_lock(&g_lock);
    SLresult result;
    result = (*bqPlayerPlay)->SetPlayState(bqPlayerPlay, SL_PLAYSTATE_STOPPED);
    assert(SL_RESULT_SUCCESS == result);

    memset(buffer, 0, BUFFER_SIZE * g_audioFormat.sampleOffset);
    bufferLen[0] = BUFFER_SIZE_IN_SAMPLES;
    bufferLen[1] = BUFFER_SIZE_IN_SAMPLES;

    if (bqPlayerObject != nullptr) {
        (*bqPlayerObject)->Destroy(bqPlayerObject);
        bqPlayerObject = nullptr;
        bqPlayerPlay = nullptr;
        bqPlayerBufferQueue = nullptr;
        bqPlayerMuteSolo = nullptr;
        bqPlayerVolume = nullptr;
    }
    if (outputMixObject != nullptr) {
        (*outputMixObject)->Destroy(outputMixObject);
        outputMixObject = nullptr;
    }
    if (engineObject != nullptr) {
        (*engineObject)->Destroy(engineObject);
        engineObject = nullptr;
        engineEngine = nullptr;
    }
    pthread_mutex_unlock(&g_lock);
}

/************************************************************************************************
 ********************** Minimal OpenSL ES wrapper implementation END ****************************
 ************************************************************************************************/

int audioCallbackFunction(sample_t *outBuffer, int num_samples)
{
    OPN2_UInt8 *buff = reinterpret_cast<OPN2_UInt8*>(outBuffer);
    int ret = opn2_playFormat(playingDevice, num_samples,
            buff, buff + g_audioFormat.containerSize,
            &g_audioFormat);

    if((g_gaining > 0.1) && (g_gaining != 1.0))
    {
        if(g_audioFormat.type == OPNMIDI_SampleType_F32)
        {
            float *bu = (float *) outBuffer;
            for(size_t i = 0; i < num_samples; i++)
            {
                *bu *= (float)g_gaining;
                bu++;
            }
        }
        else
        {
            int16_t *bu = (int16_t *) outBuffer;
            for(size_t i = 0; i < num_samples; i++)
            {
                *bu = static_cast<int16_t>(static_cast<double>(*bu) * g_gaining);
                bu++;
            }
        }
    }

    return ret;
}

void infiniteLoopStream(OPN2_MIDIPlayer* device)
{
    if(mutex_created)
    {
        assert(pthread_mutex_init(&g_lock, nullptr) == 0);
        mutex_created=true;
    }
    playingDevice = device;
    pthread_mutex_lock(&g_lock);
    OpenSLWrap_Init(audioCallbackFunction);
    pthread_mutex_unlock(&g_lock);
}




#define ADLDEV (OPN2_MIDIPlayer*)device

JNIEXPORT void JNICALL
Java_ru_wohlsoft_opnmidiplayer_PlayerService_startPlaying(JNIEnv *env, jobject instance, jlong device)
{
    infiniteLoopStream(ADLDEV);
}

JNIEXPORT void JNICALL
Java_ru_wohlsoft_opnmidiplayer_PlayerService_stopPlaying(JNIEnv *env, jobject instance)
{
    OpenSLWrap_Shutdown();
}


JNIEXPORT jstring JNICALL
Java_ru_wohlsoft_opnmidiplayer_PlayerService_adl_1errorString(JNIEnv *env, jobject instance)
{
    pthread_mutex_lock(&g_lock);
    const char* adlMIDIerr = opn2_errorString();
    pthread_mutex_unlock(&g_lock);
    return env->NewStringUTF(adlMIDIerr);
}

JNIEXPORT void JNICALL
Java_ru_wohlsoft_opnmidiplayer_PlayerService_setGaining(JNIEnv *env, jobject /*instance*/, jdouble gaining)
{
    pthread_mutex_lock(&g_lock);
    g_gaining = (double)gaining;
    pthread_mutex_unlock(&g_lock);
}

JNIEXPORT jstring JNICALL
Java_ru_wohlsoft_opnmidiplayer_PlayerService_adl_1errorInfo(JNIEnv *env, jobject instance, jlong device) {

    pthread_mutex_lock(&g_lock);
    const char* adlMIDIerr = opn2_errorInfo(ADLDEV);
    pthread_mutex_unlock(&g_lock);
    return env->NewStringUTF(adlMIDIerr);
}

JNIEXPORT jstring JNICALL
Java_ru_wohlsoft_opnmidiplayer_PlayerService_stringFromJNI(JNIEnv *env, jobject /* this */)
{
    std::string hello = "OPN2 Emulator is ready";
    return env->NewStringUTF(hello.c_str());
}

JNIEXPORT jint JNICALL
Java_ru_wohlsoft_opnmidiplayer_PlayerService_adl_1setEmulator(JNIEnv * /*env*/, jclass /*type*/,
                                                              jlong device, jint emulator)
{
    pthread_mutex_lock(&g_lock);
    jint ret = (jint)opn2_switchEmulator(ADLDEV, (int)emulator);
    pthread_mutex_unlock(&g_lock);
    return ret;
}

JNIEXPORT jint JNICALL
Java_ru_wohlsoft_opnmidiplayer_PlayerService_adl_1setNumChips(JNIEnv *env, jobject instance, jlong device,
                                                       jint numCards) {
    pthread_mutex_lock(&g_lock);
    jint ret = (jint)opn2_setNumChips(ADLDEV, (int)numCards);
    pthread_mutex_unlock(&g_lock);
    return ret;
}

JNIEXPORT jlong JNICALL
Java_ru_wohlsoft_opnmidiplayer_PlayerService_adl_1init(JNIEnv *env, jobject instance, jlong sampleRate)
{
    return (jlong)opn2_init((long)sampleRate);
}

JNIEXPORT void JNICALL
Java_ru_wohlsoft_opnmidiplayer_PlayerService_adl_1setScaleModulators(JNIEnv *env, jobject instance,
                                                              jlong device, jint smod)
{
    pthread_mutex_lock(&g_lock);
    opn2_setScaleModulators(ADLDEV, (int)smod);
    pthread_mutex_unlock(&g_lock);
}

JNIEXPORT void JNICALL
Java_ru_wohlsoft_opnmidiplayer_PlayerService_adl_1setFullRangeBrightness(JNIEnv *env, jobject instance,
                                                              jlong device, jint fr_brightness)
{
    pthread_mutex_lock(&g_lock);
    opn2_setFullRangeBrightness(ADLDEV, (int)fr_brightness);
    pthread_mutex_unlock(&g_lock);
}


JNIEXPORT void JNICALL
Java_ru_wohlsoft_opnmidiplayer_PlayerService_adl_1setLoopEnabled(JNIEnv *env, jobject instance,
                                                          jlong device, jint loopEn)
{
    pthread_mutex_lock(&g_lock);
    opn2_setLoopEnabled(ADLDEV, (int)loopEn);
    pthread_mutex_unlock(&g_lock);
}


JNIEXPORT jint JNICALL
Java_ru_wohlsoft_opnmidiplayer_PlayerService_adl_1openBankFile(JNIEnv *env, jobject instance, jlong device,
                                                    jstring file_) {
    pthread_mutex_lock(&g_lock);
    const char *file = env->GetStringUTFChars(file_, 0);
    int ret = opn2_openBankFile(ADLDEV, (char*)file);
    env->ReleaseStringUTFChars(file_, file);
    pthread_mutex_unlock(&g_lock);
    return ret;
}

JNIEXPORT jint JNICALL
Java_ru_wohlsoft_opnmidiplayer_PlayerService_adl_1openBankData(JNIEnv *env, jobject instance, jlong device,
                                                    jbyteArray array_) {
    pthread_mutex_lock(&g_lock);
    jbyte *array = env->GetByteArrayElements(array_, nullptr);
    jsize length =  env->GetArrayLength(array_);
    jint ret = opn2_openBankData(ADLDEV, array, length);
    env->ReleaseByteArrayElements(array_, array, 0);
    pthread_mutex_unlock(&g_lock);
    return ret;
}

JNIEXPORT jint JNICALL
Java_ru_wohlsoft_opnmidiplayer_PlayerService_adl_1openFile(JNIEnv *env, jobject instance, jlong device,
                                                    jstring file_) {
    pthread_mutex_lock(&g_lock);
    const char *file = env->GetStringUTFChars(file_, 0);
    int ret = opn2_openFile(ADLDEV, (char*)file);
    env->ReleaseStringUTFChars(file_, file);
    pthread_mutex_unlock(&g_lock);
    return ret;
}

JNIEXPORT jint JNICALL
Java_ru_wohlsoft_opnmidiplayer_PlayerService_adl_1openData(JNIEnv *env, jobject instance, jlong device,
                                                    jbyteArray array_) {
    pthread_mutex_lock(&g_lock);
    jbyte *array = env->GetByteArrayElements(array_, nullptr);
    jsize length =  env->GetArrayLength(array_);
    jint ret = opn2_openData(ADLDEV, array, static_cast<unsigned long>(length));
    env->ReleaseByteArrayElements(array_, array, 0);
    pthread_mutex_unlock(&g_lock);
    return ret;
}

JNIEXPORT void JNICALL
Java_ru_wohlsoft_opnmidiplayer_PlayerService_adl_1reset(JNIEnv *env, jobject instance, jlong device) {
    pthread_mutex_lock(&g_lock);
    opn2_reset(ADLDEV);
    pthread_mutex_unlock(&g_lock);
}

JNIEXPORT void JNICALL
Java_ru_wohlsoft_opnmidiplayer_PlayerService_adl_1close(JNIEnv *env, jobject instance, jlong device)
{
    pthread_mutex_lock(&g_lock);
    opn2_close(ADLDEV);
    pthread_mutex_unlock(&g_lock);
}

JNIEXPORT jint JNICALL
Java_ru_wohlsoft_opnmidiplayer_PlayerService_adl_1play(JNIEnv *env, jobject instance, jlong device,
                                                jshortArray buffer_)
{
    pthread_mutex_lock(&g_lock);
    jshort *buffer = env->GetShortArrayElements(buffer_, nullptr);
    jsize  length =  env->GetArrayLength(buffer_);
    short* outBuff = reinterpret_cast<short*>(buffer);
    jint gotSamples = opn2_play(ADLDEV, length, outBuff);
    env->ReleaseShortArrayElements(buffer_, buffer, 0);
    pthread_mutex_unlock(&g_lock);
    return gotSamples;
}


JNIEXPORT jdouble JNICALL
Java_ru_wohlsoft_opnmidiplayer_PlayerService_adl_1positionTell(JNIEnv *env, jobject instance, jlong device)
{
    pthread_mutex_lock(&g_lock);
    jdouble ret = static_cast<jdouble>(opn2_positionTell(ADLDEV));
    pthread_mutex_unlock(&g_lock);
    return ret;
}

JNIEXPORT jdouble JNICALL
Java_ru_wohlsoft_opnmidiplayer_PlayerService_adl_1totalTimeLength(JNIEnv *env, jobject instance, jlong device)
{
    pthread_mutex_lock(&g_lock);
    jdouble ret = (jdouble)opn2_totalTimeLength(ADLDEV);
    pthread_mutex_unlock(&g_lock);
    return ret;
}

JNIEXPORT void JNICALL
Java_ru_wohlsoft_opnmidiplayer_PlayerService_adl_1positionSeek(JNIEnv *env, jobject instance, jlong device,
                                                        jdouble seconds)
{
    pthread_mutex_lock(&g_lock);
    opn2_positionSeek(ADLDEV, (double)seconds);
    pthread_mutex_unlock(&g_lock);
}


JNIEXPORT jstring JNICALL
Java_ru_wohlsoft_opnmidiplayer_PlayerService_adl_1getBankName(JNIEnv *env, jobject instance, jint bank)
{
    std::string hello = "NoBanks";
    return env->NewStringUTF(hello.c_str());
}

JNIEXPORT void JNICALL
Java_ru_wohlsoft_opnmidiplayer_PlayerService_adl_1setVolumeRangeModel(JNIEnv *env, jobject instance,
                                                               jlong device, jint volumeModel) {
    pthread_mutex_lock(&g_lock);
    opn2_setVolumeRangeModel(ADLDEV, (int)volumeModel);
    pthread_mutex_unlock(&g_lock);
}


JNIEXPORT jint JNICALL
Java_ru_wohlsoft_opnmidiplayer_PlayerService_adl_1setRunAtPcmRate(JNIEnv *env, jobject instance,
                                                                 jlong device, jint enabled)
{
    pthread_mutex_lock(&g_lock);
    jint ret = (jint)opn2_setRunAtPcmRate(ADLDEV, (int)enabled);
    pthread_mutex_unlock(&g_lock);
    return ret;
}

JNIEXPORT void JNICALL
Java_ru_wohlsoft_opnmidiplayer_PlayerService_adl_1setSoftPanEnabled(JNIEnv *env, jobject /*instance*/,
                                                                  jlong device, jint enabled)
{
    pthread_mutex_lock(&g_lock);
    opn2_setSoftPanEnabled(ADLDEV, (int)enabled);
    pthread_mutex_unlock(&g_lock);
}
