#pragma once

#include "IVoiceCodec.h"

// decState for the standalone SKP_Silk SDK decoder.
extern "C" {
#include "SKP_Silk_SDK_API.h"
}

// Raw SKP_Silk packets WITHOUT the Steam P2P wrapper -- the voice format
// the old GoldSrc engine itself used ("voice_silk" era, pre-P2P), and
// which no-steam server scenes still record into their HLTV demos today.
// A svc_voicedata payload of this family is one complete SILK packet:
// a leading TOC (sampling frequency + frame count) followed by that many
// 20 ms frames, typically 38 CBR bytes each (quality 5 ~= 15.2 kbit/s).
// The SKP SDK decodes one 20 ms frame per SKP_Silk_SDK_Decode call and
// keeps the rest of the packet buffered internally (moreInternalDecoder
// Frames flag), so Decompress must loop until the packet is drained.
//
// The SDK's highest API output rate is 24000 Hz, so each 20 ms frame
// (480 samples @ 24 kHz) is linearly upsampled x2 into 960 samples at
// 48 kHz to match the rest of the voice pipeline.
class VoiceCodec_RawSilk : public IVoiceCodec
{
public:
	VoiceCodec_RawSilk();
	~VoiceCodec_RawSilk();

	bool Init(int quality) override;
	void Release() override;
	bool ResetState() override;

	int Compress(const char *pUncompressedBytes, int nSamples, char *pCompressed, int maxCompressedBytes, bool bFinal) override;
	int Decompress(const char *pCompressed, int compressedBytes, char *pUncompressed, int maxUncompressedBytes) override;

private:
	void *m_pDecoderState;        // SKP_Silk decoder state buffer
	SKP_int32 m_nDecoderStateSize;
};
