#include "header.h"

size_t ENCODED_FRAME_SIZE[] = { 0x6u, 0x6u, 0xFu, 0xFu, 0x14u, 0x14u, 0x1Cu, 0x1Cu, 0x26u, 0x26u, 0x26u };

VoiceEncoder_Speex::VoiceEncoder_Speex()
{
	m_EncoderState = nullptr;
	m_DecoderState = nullptr;
	m_Quality = 0;
	m_Bits = {};
}

VoiceEncoder_Speex::~VoiceEncoder_Speex()
{
	TermStates();
}

bool VoiceEncoder_Speex::Init(int quality, int &rawFrameSize, int &encodedFrameSize)
{
	int postfilter;
	int samplerate;

	if (!InitStates())
		return false;

	// Speex narrowband decodes 20 ms frames of 160 samples at 8 kHz. The
	// surrounding pipeline (VoiceRipper / ReVoiceDecoder alike) treats
	// codec output as 48 kHz, so one decoded frame must become 960
	// samples = 1920 bytes, playing back at exactly the 20 ms it lasted
	// in game. Without this x6 upsample the legacy-speex path produces
	// "chipmunk" audio running 6x too fast (160 samples @ 48 kHz is only
	// 3.3 ms) -- the original vendored glue had both this bug AND an
	// inverted DecodeFrame success contract, which together made the
	// whole voice_speex family output nothing at all.
	rawFrameSize = 960 * 2;

	switch (quality) {
		case 2:
			m_Quality = 2;
			break;
		case 3:
			m_Quality = 4;
			break;
		case 4:
			m_Quality = 6;
			break;
		case 5:
			m_Quality = 8;
			break;
		default:
			m_Quality = 0;
			break;
	}

	encodedFrameSize = ENCODED_FRAME_SIZE[m_Quality];

	speex_encoder_ctl(m_EncoderState, SPEEX_SET_QUALITY, &m_Quality);
	speex_decoder_ctl(m_DecoderState, SPEEX_SET_QUALITY, &m_Quality);

	postfilter = 1;
	speex_decoder_ctl(m_DecoderState, SPEEX_SET_ENH, &postfilter);

	samplerate = 48000;
	speex_decoder_ctl(m_DecoderState, SPEEX_SET_SAMPLING_RATE, &samplerate);
	speex_encoder_ctl(m_EncoderState, SPEEX_SET_SAMPLING_RATE, &samplerate);

	return true;
}

void VoiceEncoder_Speex::Release()
{
	delete this;
}

void VoiceEncoder_Speex::EncodeFrame(const char *pUncompressedBytes, char *pCompressed)
{
	float input[160];
	int16_t *in = (int16_t *)pUncompressedBytes;

	for (int i = 0; i < ARRAYSIZE(input); i++, in++) {
		input[i] = *in;
	}

	speex_bits_reset(&m_Bits);
	speex_encode(m_EncoderState, input, &m_Bits);
	speex_bits_write(&m_Bits, pCompressed, ENCODED_FRAME_SIZE[m_Quality]);
}

int VoiceEncoder_Speex::DecodeFrame(const char *pCompressed, char *pDecompressedBytes)
{
	float output[160];

	speex_bits_read_from(&m_Bits, (char *)pCompressed, ENCODED_FRAME_SIZE[m_Quality]);

	// VoiceCodec_Frame::Decompress expects the IFrameEncoder contract
	// "return 0 on success" (its loop is `if (DecodeFrame(...) != 0)
	// return 0;`); speex_decode returns 0 for success and negative for a
	// corrupt stream -- exactly that contract. The original vendored glue
	// negated it into 1-on-success, which made VoiceCodec_Frame treat
	// every SUCCESSFUL speex frame as a failure: the whole legacy-speex
	// path never produced a single sample, which is why all "should be
	// plain voice_speex" demos fell through to other codecs and either
	// failed or produced garbage.
	if (speex_decode(m_DecoderState, &m_Bits, output) != 0)
		return -1;

	// Linear x6 upsample 8 kHz -> 48 kHz (speech content lives well below
	// 4 kHz, so interpolation keeps it fully intelligible) so one 20 ms
	// frame occupies 960 output samples = exactly 20 ms at the pipeline
	// rate.
	int16_t *out = (int16_t *)pDecompressedBytes;
	for (int i = 0; i < 160; i++) {
		int16_t s = (int16_t)output[i];
		for (int sub = 0; sub < 6; sub++) {
			int next = (i + 1 < 160) ? (int)output[i + 1] : (int)s;
			out[i * 6 + sub] = (int16_t)(((int)s * (6 - sub) + next * sub) / 6);
		}
	}
	return 0;
}

bool VoiceEncoder_Speex::ResetState()
{
	speex_encoder_ctl(m_EncoderState, SPEEX_RESET_STATE, 0);
	speex_decoder_ctl(m_DecoderState, SPEEX_RESET_STATE, 0);

	return true;
}

bool VoiceEncoder_Speex::InitStates()
{
	speex_bits_init(&m_Bits);
	m_EncoderState = speex_encoder_init(&speex_nb_mode);
	m_DecoderState = speex_decoder_init(&speex_nb_mode);
	return (m_EncoderState != nullptr && m_DecoderState != nullptr);
}

void VoiceEncoder_Speex::TermStates()
{
	if (m_EncoderState) {
		speex_encoder_destroy(m_EncoderState);
		m_EncoderState = nullptr;
	}

	if (m_DecoderState) {
		speex_encoder_destroy(m_DecoderState);
		m_DecoderState = nullptr;
	}

	speex_bits_destroy(&m_Bits);
}
