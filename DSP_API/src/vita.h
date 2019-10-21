/* *****************************************************************************
 *  vita.h                                                          2014 AUG 31
 *
 *      Describes VITA 49 structures
 *
 *  \date 2012-03-28
 *  \author Eric Wachsmann, KE5DTO
 *
 * *****************************************************************************
 *
 *  Copyright (C) 2012-2014 FlexRadio Systems.
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 *  Contact Information:
 *  email: gpl<at>flexradiosystems.com
 *  Mail:  FlexRadio Systems, Suite 1-150, 4616 W. Howard LN, Austin, TX 78728
 *
 * ************************************************************************** */

#ifndef _VITA_H
#define _VITA_H

#include <linux/if_ether.h>

/* Packet Header Definitions */
#define VITA_HEADER_PACKET_TYPE_MASK                0xF0000000
#define VITA_PACKET_TYPE_IF_DATA                    0x00000000
#define VITA_PACKET_TYPE_IF_DATA_WITH_STREAM_ID     0x10000000
#define VITA_PACKET_TYPE_EXT_DATA                   0x20000000
#define VITA_PACKET_TYPE_CONTEXT                    0x40000000
#define VITA_PACKET_TYPE_EXT_CONTEXT                0x50000000

#define VITA_HEADER_C_MASK                          0x08000000
#define VITA_HEADER_CLASS_ID_PRESENT                0x08000000

#define VITA_HEADER_T_MASK                          0x04000000
#define VITA_HEADER_TRAILER_PRESENT                 0x04000000

#define VITA_HEADER_TSI_MASK                        0x00C00000
#define VITA_TSI_NONE                               0x00000000
#define VITA_TSI_UTC                                0x00400000
#define VITA_TSI_GPS                                0x00800000
#define VITA_TSI_OTHER                              0x00C00000

#define VITA_HEADER_TSF_MASK                        0x00300000
#define VITA_TSF_NONE                               0x00000000
#define VITA_TSF_SAMPLE_COUNT                       0x00100000
#define VITA_TSF_REAL_TIME                          0x00200000
#define VITA_TSF_FREE_RUNNING                       0x00300000

#define VITA_HEADER_PACKET_COUNT_MASK               0x000F0000
#define VITA_HEADER_PACKET_SIZE_MASK                0x0000FFFF

#define VITA_CLASS_ID_OUI_MASK                      0x00FFFFFF
#define VITA_CLASS_ID_INFORMATION_CLASS_MASK        0xFFFF0000
#define VITA_CLASS_ID_PACKET_CLASS_MASK             0x0000FFFF

#pragma pack(4)

// 16 ip header
#define MAX_TCP_DATA_SIZE (ETH_DATA_LEN - 16)

// 16 ip header, 6 udp header
#define MAX_UDP_DATA_SIZE (2048)

#define VITA_PACKET_TYPE_EXT_DATA_WITH_STREAM_ID	0x38

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define VITA_OUI_MASK 			0xffffff00
#define FLEX_OUI 				0x2d1c0000LL
#define DISCOVERY_CLASS_ID		((0xffff4c53LL << 32) | FLEX_OUI)
#define DISCOVERY_STREAM_ID		0x00080000
#define STREAM_BITS_IN			0x00000080
#define STREAM_BITS_OUT		    0x00000000
#define STREAM_BITS_METER		0x00000008
#define STREAM_BITS_WAVEFORM    0x00000001
#define METER_STREAM_ID         0x00070000
#define METER_CLASS_ID          ((0x02804c53LL << 32) | FLEX_OUI)
#else
#define VITA_OUI_MASK 			0x00ffffff
#define FLEX_OUI 				0x00001c2dLL
#define DISCOVERY_CLASS_ID		((FLEX_OUI << 32) | 0x534cffffLL)
#define DISCOVERY_STREAM_ID		0x00000800
#define STREAM_BITS_IN			0x80000000
#define STREAM_BITS_OUT		    0x00000000
#define STREAM_BITS_METER		0x08000000
#define STREAM_BITS_WAVEFORM    0x01000000
#define METER_STREAM_ID         0x00000700
#define METER_CLASS_ID          ((FLEX_OUI << 32) | 0x534c8002LL)
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
		uint8_t raw_payload[1024];
		uint32_t if_samples[256];
		struct {
			uint16_t id;
			uint16_t value;
		} meter[256];
	} payload;
};

struct vita_packet_without_timestamp {
    uint8_t packet_type;
    uint8_t timestamp_type;
    uint16_t length;
    uint32_t stream_id;
    uint64_t class_id;
    union {
        uint8_t raw_payload[1024];
        uint32_t if_samples[256];
        struct {
            uint16_t id;
            uint16_t value;
        } meter[256];
    } payload;
};
#pragma pack(pop)
#define VITA_PACKET_HEADER_SIZE (sizeof(struct vita_packet) - sizeof((struct vita_packet){0}.payload))
#define VITA_PACKET_HEADER_SIZE_NEW(x) (sizeof((x)) - sizeof((x).payload))

#define MAX_IF_DATA_PAYLOAD_SIZE (MAX_UDP_DATA_SIZE) //-28)
// #define MAX_IF_DATA_PAYLOAD_SIZE (ETH_DATA_LEN - 60 - 8 - sizeof(struct vita_header)) //  Max frame minus max IP header minus max UDP Header
typedef struct _vita_if_data
{
    uint32_t header;
    uint32_t stream_id;
    uint32_t class_id_h;
    uint32_t class_id_l;
    uint32_t timestamp_int;
    uint32_t timestamp_frac_h;
    uint32_t timestamp_frac_l;
    uint8_t  payload[MAX_IF_DATA_PAYLOAD_SIZE];
} vita_if_data, *VitaIFData;

#define MAX_METER_DATA_PAYLOAD_SIZE (MAX_UDP_DATA_SIZE) //-28)
typedef struct _vita_meter_data
{
    uint32_t header;
    uint32_t stream_id;
    uint32_t class_id_h;
    uint32_t class_id_l;
    uint32_t timestamp_int;
    uint32_t timestamp_frac_h;
    uint32_t timestamp_frac_l;
    uint8_t  payload[MAX_METER_DATA_PAYLOAD_SIZE];
} vita_meter_data, *VitaMeterData;

#define MAX_METER_PAYLOAD_SIZE (MAX_UDP_DATA_SIZE) //-36)
typedef struct _vita_ext_data_meter
{
    uint32_t header;
    uint32_t streamID;
    uint32_t classID_1;
    uint32_t classID_2;
    uint32_t integer_seconds;
    uint64_t frac_seconds;
    uint32_t extended_packet_type; // = 1 is meter
    uint32_t number_of_meters;
    uint8_t  payload[MAX_METER_PAYLOAD_SIZE];
} vita_ext_data_meter, *VitaExtDataMeter;

typedef struct _vita_timestamp
{
    uint32_t ts_int;
    union
    {
        struct
        {
            uint32_t ts_frac_h;
            uint32_t ts_frac_l;
        };
        uint64_t ts_frac;
    };

} vita_timestamp, *VitaTimestamp;

typedef uint32_t vita_date;

#pragma pack()

#endif /* _VITA_H */
