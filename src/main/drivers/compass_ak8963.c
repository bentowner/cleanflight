/*
 * This file is part of Cleanflight.
 *
 * Cleanflight is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Cleanflight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Cleanflight.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdbool.h>
#include <stdint.h>

#include <math.h>

#include "build_config.h"

#include "platform.h"

#include "debug.h"

#include "common/axis.h"
#include "common/maths.h"

#include "system.h"
#include "gpio.h"
#include "exti.h"
#include "bus_i2c.h"

#include "sensors/boardalignment.h"
#include "sensors/sensors.h"

#include "sensor.h"
#include "compass.h"

#include "accgyro.h"
#include "accgyro_mpu.h"
#include "accgyro_spi_mpu6500.h"
#include "compass_ak8963.h"

// This sensor is available in MPU-9250.

// AK8963, mag sensor address
#define AK8963_MAG_I2C_ADDRESS          0x0C
#define AK8963_Device_ID                0x48

// Registers
#define AK8963_MAG_REG_WHO_AM_I         0x00
#define AK8963_MAG_REG_INFO             0x01
#define AK8963_MAG_REG_STATUS1          0x02
#define AK8963_MAG_REG_HXL              0x03
#define AK8963_MAG_REG_HXH              0x04
#define AK8963_MAG_REG_HYL              0x05
#define AK8963_MAG_REG_HYH              0x06
#define AK8963_MAG_REG_HZL              0x07
#define AK8963_MAG_REG_HZH              0x08
#define AK8963_MAG_REG_STATUS2          0x09
#define AK8963_MAG_REG_CNTL             0x0a
#define AK8963_MAG_REG_ASCT             0x0c // self test
#define AK8963_MAG_REG_ASAX             0x10 // Fuse ROM x-axis sensitivity adjustment value
#define AK8963_MAG_REG_ASAY             0x11 // Fuse ROM y-axis sensitivity adjustment value
#define AK8963_MAG_REG_ASAZ             0x12 // Fuse ROM z-axis sensitivity adjustment value

#define READ_FLAG                       0x80

#define STATUS1_DATA_READY              0x01
#define STATUS1_DATA_OVERRUN            0x02

#define STATUS2_DATA_ERROR              0x02
#define STATUS2_MAG_SENSOR_OVERFLOW     0x03

#define CNTL_MODE_POWER_DOWN            0x00
#define CNTL_MODE_ONCE                  0x01
#define CNTL_MODE_CONT1                 0x02
#define CNTL_MODE_CONT2                 0x06
#define CNTL_MODE_SELF_TEST             0x08
#define CNTL_MODE_FUSE_ROM              0x0F

typedef bool (*ak8963ReadRegisterFunc)(uint8_t addr_, uint8_t reg_, uint8_t len_, uint8_t *buf);
typedef bool (*ak8963WriteRegisterFunc)(uint8_t addr_, uint8_t reg_, uint8_t data);

typedef struct ak8963Configuration_s {
    ak8963ReadRegisterFunc read;
    ak8963WriteRegisterFunc write;
} ak8963Configuration_t;

ak8963Configuration_t ak8963config;
static float magGain[3] = { 1.0f, 1.0f, 1.0f };

// FIXME pretend we have real MPU9250 support
#ifdef MPU6500_SPI_INSTANCE
#define MPU9250_SPI_INSTANCE
#define verifympu9250WriteRegister mpu6500WriteRegister
#define mpu9250WriteRegister mpu6500WriteRegister
#define mpu9250ReadRegister mpu6500ReadRegister
#endif

#ifdef USE_SPI
bool ak8963SPIRead(uint8_t addr_, uint8_t reg_, uint8_t len_, uint8_t *buf)
{
    mpu6500WriteRegister(MPU_RA_I2C_SLV0_ADDR, addr_ | READ_FLAG);   // set I2C slave address for read
    mpu6500WriteRegister(MPU_RA_I2C_SLV0_REG, reg_);                 // set I2C slave register
    mpu6500WriteRegister(MPU_RA_I2C_SLV0_CTRL, len_ | 0x80);         // read number of bytes
    delay(8);
    __disable_irq();
    mpu6500ReadRegister(MPU_RA_EXT_SENS_DATA_00, len_, buf);         // read I2C
    __enable_irq();
    return true;
}

bool ak8963SPIWrite(uint8_t addr_, uint8_t reg_, uint8_t data)
{
    mpu6500WriteRegister(MPU_RA_I2C_SLV0_ADDR, addr_);               // set I2C slave address for write
    mpu6500WriteRegister(MPU_RA_I2C_SLV0_REG, reg_);                 // set I2C slave register
    mpu6500WriteRegister(MPU_RA_I2C_SLV0_DO, data);                  // set I2C salve value
    mpu6500WriteRegister(MPU_RA_I2C_SLV0_CTRL, 0x81);                // write 1 byte
    return true;
}
#endif

#ifdef SPRACINGF3EVO

typedef struct queuedReadState_s {
    bool waiting;
    uint8_t len;
    uint32_t readStartedAt; // time read was queued in micros.
} queuedReadState_t;

static queuedReadState_t queuedRead = { false, 0, 0};

bool ak8963SPIStartRead(uint8_t addr_, uint8_t reg_, uint8_t len_)
{
    if (queuedRead.waiting) {
        return false;
    }

    queuedRead.len = len_;

    verifympu9250WriteRegister(MPU_RA_I2C_SLV0_ADDR, addr_ | READ_FLAG);   // set I2C slave address for read
    verifympu9250WriteRegister(MPU_RA_I2C_SLV0_REG, reg_);                 // set I2C slave register
    verifympu9250WriteRegister(MPU_RA_I2C_SLV0_CTRL, len_ | 0x80);         // read number of bytes

    queuedRead.readStartedAt = micros();
    queuedRead.waiting = true;

    return true;
}

static uint32_t ak8963SPIQueuedReadTimeRemaining(void)
{
    if (!queuedRead.waiting) {
        return 0;
    }

    int32_t timeSinceStarted = micros() - queuedRead.readStartedAt;

    int32_t timeRemaining = 8000 - timeSinceStarted;

    if (timeRemaining < 0) {
        return 0;
    }

    return timeRemaining;
}

bool ak8963SPICompleteRead(uint8_t *buf)
{
    uint32_t timeRemaining = ak8963SPIQueuedReadTimeRemaining();

    if (timeRemaining > 0) {
        delayMicroseconds(timeRemaining);
    }

    queuedRead.waiting = false;

    mpu9250ReadRegister(MPU_RA_EXT_SENS_DATA_00, queuedRead.len, buf);               // read I2C buffer
    return true;
}

#endif

#ifdef USE_I2C
bool c_i2cWrite(uint8_t addr_, uint8_t reg_, uint8_t data) 
{
    return i2cWrite(MAG_I2C_INSTANCE, addr_, reg_, data);
}

bool c_i2cRead(uint8_t addr_, uint8_t reg_, uint8_t len, uint8_t* buf) 
{
	return i2cRead(MAG_I2C_INSTANCE, addr_, reg_, len, buf);
}
#endif

bool ak8963Detect(mag_t *mag)
{
    bool ack = false;
    uint8_t sig = 0;

#ifdef USE_I2C
    // check for AK8963 on I2C bus
    ack = i2cRead(MAG_I2C_INSTANCE, AK8963_MAG_I2C_ADDRESS, AK8963_MAG_REG_WHO_AM_I, 1, &sig);
    if (ack && sig == AK8963_Device_ID) // 0x48 / 01001000 / 'H'
    {
        ak8963config.read = c_i2cRead;
        ak8963config.write = c_i2cWrite;
        mag->init = ak8963Init;
        mag->read = ak8963Read;

        return true;
    }
#endif

#ifdef USE_SPI
    // check for AK8963 on I2C master via SPI bus (as part of MPU9250)

    ack = mpu6500WriteRegister(MPU_RA_INT_PIN_CFG, 0x10);            // INT_ANYRD_2CLEAR
    delay(10);

    ack = mpu6500WriteRegister(MPU_RA_I2C_MST_CTRL, 0x0D);           // I2C multi-master / 400kHz
    delay(10);

    ack = mpu6500WriteRegister(MPU_RA_USER_CTRL, 0x30);              // I2C master mode, SPI mode only
    delay(10);

    ack = ak8963SPIRead(AK8963_MAG_I2C_ADDRESS, AK8963_MAG_REG_WHO_AM_I, 1, &sig);
    if (ack && sig == AK8963_Device_ID) // 0x48 / 01001000 / 'H'
    {
        ak8963config.read = ak8963SPIRead;
        ak8963config.write = ak8963SPIWrite;
        mag->init = ak8963Init;
        mag->read = ak8963Read;

        return true;
    }
#endif
    return false;
}

void ak8963Init()
{
    bool ack;
    UNUSED(ack);
    uint8_t calibration[3];
    uint8_t status;

    ack = ak8963config.write(AK8963_MAG_I2C_ADDRESS, AK8963_MAG_REG_CNTL, CNTL_MODE_POWER_DOWN); // power down before entering fuse mode
    delay(20);

    ack = ak8963config.write(AK8963_MAG_I2C_ADDRESS, AK8963_MAG_REG_CNTL, CNTL_MODE_FUSE_ROM); // Enter Fuse ROM access mode
    delay(10);

    ack = ak8963config.read(AK8963_MAG_I2C_ADDRESS, AK8963_MAG_REG_ASAX, sizeof(calibration), calibration); // Read the x-, y-, and z-axis calibration values
    delay(10);

    magGain[X] = (((((float)(int8_t)calibration[X] - 128) / 256) + 1) * 30);
    magGain[Y] = (((((float)(int8_t)calibration[Y] - 128) / 256) + 1) * 30);
    magGain[Z] = (((((float)(int8_t)calibration[Z] - 128) / 256) + 1) * 30);

    ack = ak8963config.write(AK8963_MAG_I2C_ADDRESS, AK8963_MAG_REG_CNTL, CNTL_MODE_POWER_DOWN); // power down after reading.
    delay(10);

    // Clear status registers
    ack = ak8963config.read(AK8963_MAG_I2C_ADDRESS, AK8963_MAG_REG_STATUS1, 1, &status);
    ack = ak8963config.read(AK8963_MAG_I2C_ADDRESS, AK8963_MAG_REG_STATUS2, 1, &status);

    // Trigger first measurement
#ifdef SPRACINGF3EVO
    ack = ak8963config.write(AK8963_MAG_I2C_ADDRESS, AK8963_MAG_REG_CNTL, CNTL_MODE_CONT1);
#else
    ack = ak8963config.write(AK8963_MAG_I2C_ADDRESS, AK8963_MAG_REG_CNTL, CNTL_MODE_ONCE);
#endif
}

typedef enum {
    CHECK_STATUS = 0,
    WAITING_FOR_STATUS,
    WAITING_FOR_DATA
} ak8963ReadState_e;

bool ak8963Read(int16_t *magData)
{
    bool ack;
    uint8_t buf[7];

#ifdef SPRACINGF3EVO

    // we currently need a different approach on the SPRacingF3EVO which has an MPU9250 connected via SPI.
    // we cannot use the ak8963SPIRead() method, it is to slow and blocks for far too long.

    static ak8963ReadState_e state = CHECK_STATUS;

    bool retry = true;

restart:
    switch (state) {
        case CHECK_STATUS:
            ak8963SPIStartRead(AK8963_MAG_I2C_ADDRESS, AK8963_MAG_REG_STATUS1, 1);
            state++;
            return false;

        case WAITING_FOR_STATUS: {
            uint32_t timeRemaining = ak8963SPIQueuedReadTimeRemaining();
            if (timeRemaining) {
                return false;
            }

            ack = ak8963SPICompleteRead(&buf[0]);

            uint8_t status = buf[0];

            if (!ack || ((status & STATUS1_DATA_READY) == 0 && (status & STATUS1_DATA_OVERRUN) == 0)) {
                // too early. queue the status read again
                state = CHECK_STATUS;
                if (retry) {
                    retry = false;
                    goto restart;
                }
                return false;
            }


            // read the 6 bytes of data and the status2 register
            ak8963SPIStartRead(AK8963_MAG_I2C_ADDRESS, AK8963_MAG_REG_HXL, 7);

            state++;

            return false;
        }

        case WAITING_FOR_DATA: {
            uint32_t timeRemaining = ak8963SPIQueuedReadTimeRemaining();
            if (timeRemaining) {
                return false;
            }

            ack = ak8963SPICompleteRead(&buf[0]);

            uint8_t status2 = buf[6];
            if (!ack || (status2 & STATUS2_DATA_ERROR) || (status2 & STATUS2_MAG_SENSOR_OVERFLOW)) {
                return false;
            }

            magData[X] = -(int16_t)(buf[1] << 8 | buf[0]) * magGain[X];
            magData[Y] = -(int16_t)(buf[3] << 8 | buf[2]) * magGain[Y];
            magData[Z] = -(int16_t)(buf[5] << 8 | buf[4]) * magGain[Z];

            state = CHECK_STATUS;

            return true;
        }
    }

    return false;
#else
    ack = ak8963config.read(AK8963_MAG_I2C_ADDRESS, AK8963_MAG_REG_STATUS1, 1, &buf[0]);

    uint8_t status = buf[0];

    if (!ack || (status & STATUS1_DATA_READY) == 0) {
        return false;
    }

    ack = ak8963config.read(AK8963_MAG_I2C_ADDRESS, AK8963_MAG_REG_HXL, 7, &buf[0]);

    uint8_t status2 = buf[6];
    if (!ack || (status2 & STATUS2_DATA_ERROR) || (status2 & STATUS2_MAG_SENSOR_OVERFLOW)) {
        return false;
    }

    magData[X] = -(int16_t)(buf[1] << 8 | buf[0]) * magGain[X];
    magData[Y] = -(int16_t)(buf[3] << 8 | buf[2]) * magGain[Y];
    magData[Z] = -(int16_t)(buf[5] << 8 | buf[4]) * magGain[Z];

    return ak8963config.write(AK8963_MAG_I2C_ADDRESS, AK8963_MAG_REG_CNTL, CNTL_MODE_ONCE); // start reading again
#endif
}
