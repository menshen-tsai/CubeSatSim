/*
 *  Generates telemetry for CubeSat Simulator
 *
 *  Copyright Alan B. Johnston
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *  INA219 Raspberry Pi wiringPi code is based on Adafruit Arduino wire code
 *  from https://github.com/adafruit/Adafruit_INA219.
 */

#include <fcntl.h>                              
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include "status.h"
#include "ax5043.h"
#include "ax25.h"
#include "spi/ax5043spi.h"
#include <wiringPiI2C.h>
#include <wiringPi.h>
#include <time.h>
#include <math.h>
#include "../Adafruit_INA219/Adafruit_INA219.h" // From Adafruit INA219 library for Arduino

#define A 1
#define B 2
#define C 3
#define D 4

#define PLUS_X 0
#define PLUS_Y 1
#define PLUS_Z 2
#define BAT 3
#define MINUS_X 4
#define MINUS_Y 5
#define MINUS_Z 6
#define BUS 7
#define OFF -1

int twosToInt(int val, int len);

struct SensorConfig {
    int fd;
    uint16_t  config;
    int calValue;    
    int powerMultiplier;
    int currentDivider;
};

struct SensorData {
    double current;
    double voltage;
    double power;
};

/**
 * @brief Read the data from one of the i2c current sensors.
 *
 * Reads the current data from the requested i2c current sensor configuration and
 * stores it into a SensorData struct. An invalid file descriptor (i.e. less than zero)
 * results in a SensorData struct being returned that has both its #current and #power members
 * set to NAN.
 *
 * @param sensor A structure containing sensor configuration including the file descriptor.
 * @return struct SensorData A struct that contains the current, voltage, and power readings
 * from the requested sensor.
 */
struct SensorData read_sensor_data(struct SensorConfig sensor) {
    struct SensorData data = {
        .current = NAN,
        .voltage = NAN,
        .power = NAN    };

    if (sensor.fd < 0) {
        return data;
    }
    // doesn't read negative currents accurately, shows -0.1mA	
    wiringPiI2CWriteReg16(sensor.fd, INA219_REG_CALIBRATION, sensor.calValue);
    wiringPiI2CWriteReg16(sensor.fd, INA219_REG_CONFIG, sensor.config);	
    wiringPiI2CWriteReg16(sensor.fd, INA219_REG_CALIBRATION, sensor.calValue);
    int value  = wiringPiI2CReadReg16(sensor.fd, INA219_REG_CURRENT);
    data.current  = (float) twosToInt(value, 16) / (float) sensor.currentDivider;
	
    wiringPiI2CWrite(sensor.fd, INA219_REG_BUSVOLTAGE);
    delay(1); // Max 12-bit conversion time is 586us per sample
    value = (wiringPiI2CRead(sensor.fd) << 8 ) | wiringPiI2CRead (sensor.fd);	
    data.voltage  =  ((float)(value >> 3) * 4) / 1000;	
    // power has very low resolution, seems to step in 512mW values	
    data.power   = (float) wiringPiI2CReadReg16(sensor.fd, INA219_REG_POWER) * (float) sensor.powerMultiplier;
 	
    return data;
}

/**
 * @brief Configures an i2c current sensor.
 *
 * Calculates the configuration values of the i2c sensor so that
 * current, voltage, and power can be read using read_sensor_data.
 * Supports 16V 400mA and 16V 2.0A settings.
 *
 * @param sensor A file descriptor that can be used to read from the sensor.
 * @param milliAmps The mA configuration, either 400mA or 2A are supported.
 * @return struct SensorConfig A struct that contains the configuraton of the sensor.
 */
//struct SensorConfig config_sensor(int sensor, int milliAmps) {
struct SensorConfig config_sensor(char *bus, int address,  int milliAmps) {
    struct SensorConfig data;
	
    if (access(bus, W_OK | R_OK) < 0)  {   // Test if I2C Bus is missing 
	    printf("ERROR: %s bus not present \n", bus);
	    data.fd = OFF;
	    return (data);
    }
	
    data.fd = wiringPiI2CSetupInterface(bus, address);	
	
    data.config = INA219_CONFIG_BVOLTAGERANGE_32V |
                  INA219_CONFIG_GAIN_1_40MV | 
                  INA219_CONFIG_BADCRES_12BIT |
                  INA219_CONFIG_SADCRES_12BIT_1S_532US |
                  INA219_CONFIG_MODE_SANDBVOLT_CONTINUOUS;
		 
    if (milliAmps == 400) {	// INA219 16V 400mA configuration
      data.calValue = 8192;    
      data.powerMultiplier = 1; 
      data.currentDivider = 20;  // 40; in Adafruit config
    }
    else  {                     // INA219 16V 2A configuration
      data.calValue = 40960;    
      data.powerMultiplier = 2;  
      data.currentDivider = 10;  // 20; in Adafruit config
    }	
	
    #ifdef DEBUG_LOGGING
	printf("Sensor %s %x configuration: %d %d %d %d %d\n", bus, address, data.fd,
	       data.config, data.calValue, data.currentDivider, data.powerMultiplier); 
    #endif	
    return data;
}

struct SensorConfig sensor[8];   // 7 current sensors in Solar Power PCB plus one in MoPower UPS V2
struct SensorData reading[8];   // 7 current sensors in Solar Power PCB plus one in MoPower UPS V2 
struct SensorConfig tempSensor; 

int main(int argc, char *argv[]) {
	
  if (argc > 1) {
	  ;
  }

  wiringPiSetup ();
  
  tempSensor = config_sensor("/dev/i2c-3", 0x48, 0);
	
  sensor[PLUS_X]  = config_sensor("/dev/i2c-1", 0x40, 400); 
  sensor[PLUS_Y]  = config_sensor("/dev/i2c-1", 0x41, 400);
  sensor[PLUS_Z]  = config_sensor("/dev/i2c-1", 0x44, 400);
  sensor[BAT]     = config_sensor("/dev/i2c-1", 0x45, 400);
  sensor[BUS]     = config_sensor("/dev/i2c-1", 0x4a, 2000);
  sensor[MINUS_X] = config_sensor("/dev/i2c-0", 0x40, 400);
  sensor[MINUS_Y] = config_sensor("/dev/i2c-0", 0x41, 400);
  sensor[MINUS_Z] = config_sensor("/dev/i2c-0", 0x44, 400); 

//  Reading I2C voltage and current sensors
  int count;
  for (count = 0; count < 8; count++)
  {
    reading[count] = read_sensor_data(sensor[count]);	
//    #ifdef DEBUG_LOGGING
      printf("Read sensor[%d] % 4.2fV % 6.1fmA % 6.1fmW \n", 
	        count, reading[count].voltage, reading[count].current, reading[count].power); 
//    #endif
  }

  return 0;
}


int twosToInt(int val,int len) {   // Convert twos compliment to integer
// from https://www.raspberrypi.org/forums/viewtopic.php?t=55815
	
      if(val & (1 << (len - 1)))
         val = val - (1 << len);

      return(val);
}		 
