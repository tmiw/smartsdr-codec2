

#ifndef _VITA_H
#define _VITA_H

#define VITA_PACKET_TYPE_EXT_DATA_WITH_STREAM_ID	0x38u
#define VITA_PACKET_TYPE_IF_DATA_WITH_STREAM_ID     0x18u

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define VITA_OUI_MASK 			0xffffff00u
#define FLEX_OUI 				0x2d1c0000LLU
#define DISCOVERY_CLASS_ID		((0xffff4c53LLU << 32u) | FLEX_OUI)
#define DISCOVERY_STREAM_ID		0x00080000u
#define STREAM_BITS_IN			0x00000080u
#define STREAM_BITS_OUT		    0x00000000u
#define STREAM_BITS_METER		0x00000008u
#define STREAM_BITS_WAVEFORM    0x00000001u
#define METER_STREAM_ID         0x00000088u
#define METER_CLASS_ID          ((0x02804c53LLU << 32u) | FLEX_OUI)
#define AUDIO_CLASS_ID          ((0xe3034c53LLU << 32u) | FLEX_OUI)
#else
#define VITA_OUI_MASK 			0x00ffffffu
#define FLEX_OUI 				0x00001c2dLLU
#define DISCOVERY_CLASS_ID		((FLEX_OUI << 32u) | 0x534cffffLLU)
#define DISCOVERY_STREAM_ID		0x00000800u
#define STREAM_BITS_IN			0x80000000u
#define STREAM_BITS_OUT		    0x00000000u
#define STREAM_BITS_METER		0x08000000u
#define STREAM_BITS_WAVEFORM    0x01000000u
#define METER_STREAM_ID         0x88000000u
#define METER_CLASS_ID          ((FLEX_OUI << 32u) | 0x534c8002LLU)
#define AUDIO_CLASS_ID          ((FLEX_OUT << 32u) | 0x534c03e3LLU)
#endif

#define STREAM_BITS_MASK	    (STREAM_BITS_IN | STREAM_BITS_OUT | STREAM_BITS_METER | STREAM_BITS_WAVEFORM)

#pragma pack(push, 1)
struct vita_packet {
	uint8_t packet_type;
	uint8_t timestamp_type;
	uint16_t length;
	uint32_t stream_id;
	uint64_t class_id;
	uint32_t timestamp_int;
	uint64_t timestamp_frac;
	union {
		uint8_t raw_payload[1024];  // 1024
		uint32_t if_samples[256];   // 256
		struct {
			uint16_t id;
			uint16_t value;
		} meter[360];               // 360
	};
};
#pragma pack(pop)

#define VITA_PACKET_HEADER_SIZE (sizeof(struct vita_packet) - sizeof(((struct vita_packet){0}).raw_payload))

#endif /* _VITA_H */
