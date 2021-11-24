/**
 * @file telemetry.h
 * @author Алексей Хохлов <root@amper.me>
 * @copyright WTFPL License
 * @date 2021
 * @brief Работа с телеметрией
 */

#pragma once

#include <svc/platform.h>

#define RC_TELEMETRY_MAGIC (0x5243535441545553ULL)

#define OPNAMELEN (32U)

typedef struct {
	uint64_t magic;	    //  0xB01EDEAD
	uint64_t Timestamp; // -not used- timestamp in milliseconds

	struct {
		uint16_t PackVoltageX100; // -fl voltage-
		uint16_t PackCurrentX10;  // -fl ampere-
		uint16_t mAHConsumed;	  // -u16 mahconsumed-
		uint16_t __pad;
	} power;

	struct {
		uint8_t CPUload;
		int8_t CPUtemp;
		int8_t __pad[6U];
	} system;

	/* состояние модема */
	struct {
		char OpName[OPNAMELEN];
		uint8_t Status;
		uint8_t Signal;
		uint8_t Mode;
		uint8_t __pad;
	} link;

	struct {
		int32_t LatitudeX1E7;	// -dbl latitude- (degrees * 10,000,000 )
		int32_t LongitudeX1E7;	// -dbl longitude- (degrees * 10,000,000 )
		int32_t GPSAltitudecm;	// -fl altitude- ( GPS altitude, using WGS-84 ellipsoid, cm)
		uint8_t HDOPx10;	// -fl hdop- GPS HDOP * 10
		uint8_t SatsInView;	// -u8 sats- satellites in view
		uint8_t SatsInUse;	// -u8 sats- satellites used for navigation
		uint8_t __pad;		//
		uint16_t SpeedKPHX10;	// -fl speed- ( km/h * 10 )
		uint16_t CourseDegrees; // -u16 coursedegrees- GPS course over ground, in degrees
	} gps;

	struct {
		int16_t PitchDegrees; // -i16 pitch-
		int16_t RollDegrees;  // -i16 roll-
		int16_t YawDegrees;   // -fl heading-
		uint16_t
		    CompassDegrees; // -u16 compassdegrees used- either magnetic compass reading (if
				    // compass enabled) or filtered GPS course over ground if not
	} orientation;

	uint16_t CRC;
} RC_td_t;

int telemetry_init(void);

int telemetry_main(void);