#pragma once
#define ARRAYSIZE(p)		(sizeof(p)/sizeof(p[0]))

#include <time.h>
#include <stdint.h>
#include <cstdio>
#include <algorithm>
#include "iframeencoder.h"
#include "VoiceEncoder_Silk.h"
#include "VoiceEncoder_Speex.h"
#include "VoiceEncoder_Opus.h"
#include "VoiceCodec_RawSilk.h"
#include "voice_codec_frame.h"
#include "SteamP2PCodec.h"
#include "wave_header.h"