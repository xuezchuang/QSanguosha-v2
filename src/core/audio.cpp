#include "audio.h"
#include "settings.h"
#include "fmod/fmod.hpp"
//#include "common.h"
#include "fmod/fmod_common.h"
//using namespace FMOD;
class WSound;

static FMOD_SYSTEM *System;
static QCache<QString, WSound> SoundCache;
static FMOD_SOUND *BGM;
static FMOD_CHANNEL *BGMChannel;

class WSound
{
public:
    WSound(const QString &filename) : sound(NULL), channel(NULL)
    {
        FMOD_System_CreateSound(System, filename.toLatin1(), FMOD_DEFAULT, NULL, &sound);
    }

    ~WSound()
    {
        if (sound) FMOD_Sound_Release(sound);
    }

    void play()
    {
        if (sound) {
            //FMOD_RESULT result = System->playSound(sound, 0, false, &channel);
            FMOD_RESULT result = FMOD_System_PlaySound(System, sound, 0,false, &channel);

            if (result == FMOD_OK) {
                FMOD_Channel_SetVolume(channel, Config.EffectVolume);
                FMOD_System_Update(System);
            }
        }
    }

    bool isPlaying() const
    {
        if (channel == NULL) return false;

        FMOD_BOOL is_playing = false;
        FMOD_Channel_IsPlaying(channel, &is_playing);
        return is_playing;
    }

private:
    FMOD_SOUND *sound;
    FMOD_CHANNEL *channel;
};

void Audio::init()
{
    //FMOD_RESULT result = FMOD::System_Create(&System);
    FMOD_RESULT result = FMOD_System_Create(&System);
	//void* extradriverdata = 0;

	//Common_Init(&extradriverdata);
    //if (result == FMOD_OK) result = system->init(32, FMOD_INIT_NORMAL, extradriverdata); //FMOD_System_Init(System, 100, 0, NULL);
    if (result == FMOD_OK) FMOD_System_Init(System, 100, 0, NULL);
}

void Audio::quit()
{
    if (System) {
        SoundCache.clear();
        FMOD_System_Release(System);

        System = NULL;
    }
}

void Audio::play(const QString &filename, bool superpose)
{
    WSound *sound = SoundCache[filename];
    if (sound == NULL) {
        sound = new WSound(filename);
        SoundCache.insert(filename, sound);
    } else if (!superpose && sound->isPlaying()) {
        return;
    }

    sound->play();
}

void Audio::stop()
{
    if (System == NULL) return;

    int n;
    FMOD_System_GetChannelsPlaying(System, &n,0);

    QList<FMOD_CHANNEL *> channels;
    for (int i = 0; i < n; i++) {
        FMOD_CHANNEL *channel;
        FMOD_RESULT result = FMOD_System_GetChannel(System, i, &channel);
        if (result == FMOD_OK) channels << channel;
    }

    foreach (FMOD_CHANNEL *channel, channels)
        FMOD_Channel_Stop(channel);

    stopBGM();

    FMOD_System_Update(System);
}

void Audio::playBGM(const QString &filename)
{
    FMOD_RESULT result = FMOD_System_CreateStream(System, filename.toLocal8Bit(), FMOD_LOOP_NORMAL, NULL, &BGM);

    if (result == FMOD_OK) {
        FMOD_Sound_SetLoopCount(BGM, -1);
        FMOD_System_PlaySound(System, BGM,NULL, false, &BGMChannel);

        FMOD_System_Update(System);
    }
}

void Audio::setBGMVolume(float volume)
{
    if (BGMChannel) FMOD_Channel_SetVolume(BGMChannel, volume);
}

void Audio::stopBGM()
{
    if (BGMChannel) FMOD_Channel_Stop(BGMChannel);
}

QString Audio::getVersion()
{
    unsigned int version = 0;
    FMOD_System_GetVersion(System, &version);
    // convert it to QString
    return QString("%1.%2.%3").arg((version & 0xFFFF0000) >> 16, 0, 16)
        .arg((version & 0xFF00) >> 8, 2, 16, QChar('0'))
        .arg((version & 0xFF), 2, 16, QChar('0'));
}

