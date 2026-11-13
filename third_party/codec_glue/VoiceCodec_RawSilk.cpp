#include "header.h"
#include "VoiceCodec_RawSilk.h"

VoiceCodec_RawSilk::VoiceCodec_RawSilk()
{
	m_pDecoderState = nullptr;
	m_nDecoderStateSize = 0;
}

VoiceCodec_RawSilk::~VoiceCodec_RawSilk()
{
	if (m_pDecoderState) {
		free(m_pDecoderState);
		m_pDecoderState = nullptr;
	}
}

bool VoiceCodec_RawSilk::Init(int quality)
{
	if (SKP_Silk_SDK_Get_Decoder_Size(&m_nDecoderStateSize) != 0 || m_nDecoderStateSize <= 0) {
		return false;
	}

	m_pDecoderState = malloc(static_cast<size_t>(m_nDecoderStateSize));
	if (!m_pDecoderState) {
		return false;
	}

	if (SKP_Silk_SDK_InitDecoder(m_pDecoderState) != 0) {
		free(m_pDecoderState);
		m_pDecoderState = nullptr;
		return false;
	}

	return true;
}

void VoiceCodec_RawSilk::Release()
{
	delete this;
}

bool VoiceCodec_RawSilk::ResetState()
{
	if (!m_pDecoderState) {
		return false;
	}

	SKP_Silk_SDK_InitDecoder(m_pDecoderState);
	return true;
}

int VoiceCodec_RawSilk::Compress(const char *pUncompressedBytes, int nSamples, char *pCompressed, int maxCompressedBytes, bool bFinal)
{
	// Encoding is never used (VoiceRipper only extracts voice from demos).
	return 0;
}

int VoiceCodec_RawSilk::Decompress(const char *pCompressed, int compressedBytes, char *pUncompressed, int maxUncompressedBytes)
{
	if (!m_pDecoderState || !pCompressed || compressedBytes <= 0 || !pUncompressed) {
		return 0;
	}

	// One svc_voicedata payload of this family is a CONCATENATION of
	// fully self-contained 20 ms SILK packets, each exactly 38 CBR bytes
	// (~15.2 kbit/s): the SKP SDK reports "framesPerPacket=1" for the
	// first block and moreInternalDecoderFrames=0 -- feed the chunk as
	// one big packet and the SDK silently decodes ONLY the first block
	// and throws the rest away, which turned every 266-byte (7-frame)
	// chunk into one lone 20 ms frame -- speech slowed down ~7x and
	// sounded like garbage. The fix: stride through the payload in
	// 38-byte blocks, one SDK call each, keeping the decoder state
	// (LPC history) across blocks so speech stays continuous. A block
	// that fails to decode (corrupt/garbage) is skipped, but the stride
	// stays locked to 38-byte grid so the stream can't desync.
	constexpr int kBlockBytes = 38;

	int samples48k = 0;
	int offset = 0;

	static_assert(sizeof(int16_t) == 2, "int16_t expected");
	const int maxOutSamples = maxUncompressedBytes / 2;

	while (offset + kBlockBytes <= compressedBytes) {
		SKP_SILK_SDK_DecControlStruct ctrl = {};
		ctrl.API_sampleRate = 24000;

		// 20 ms @ 24 kHz per call
		SKP_int16 decoded[480];
		SKP_int16 samplesOut = 0;

		SKP_int ret = SKP_Silk_SDK_Decode(m_pDecoderState, &ctrl, 0,
		                                  reinterpret_cast<const SKP_uint8 *>(pCompressed) + offset,
		                                  kBlockBytes,
		                                  decoded, &samplesOut);

		if (ret == 0 && samplesOut > 0 && samples48k + samplesOut * 2 <= maxOutSamples) {
			// Linear x2 upsample 24 kHz -> 48 kHz (kHz-rate speech has
			// almost no energy above 12 kHz, so LERP upsampling keeps it
			// fully intelligible without staircase imaging).
			int16_t *out = reinterpret_cast<int16_t *>(pUncompressed) + samples48k;
			for (int i = 0; i < samplesOut; i++) {
				int16_t s = decoded[i];
				out[i * 2] = s;
				if (i + 1 < samplesOut) {
					out[i * 2 + 1] = static_cast<int16_t>((static_cast<int>(s) + static_cast<int>(decoded[i + 1])) / 2);
				} else {
					out[i * 2 + 1] = s;
				}
			}
			samples48k += samplesOut * 2;
		}

		offset += kBlockBytes;
	}

	return samples48k;
}
