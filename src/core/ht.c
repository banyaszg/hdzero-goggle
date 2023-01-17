#include <stdio.h>
#include <math.h>
#include <memory.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <string.h>
#include <errno.h>

#include <minIni.h>
#include <log/log.h>

#include "common.hh"
#include "ht.h"
#include "osd.h"
#include "MadgwickAHRS.h"
#include "../bmi270/accel_gyro.h"
#include "../driver/hardware.h"
#include "../driver/dm6302.h"
#include "../driver/oled.h"
#include "ui/page_common.h"
#include "ui/page_scannow.h"


//#define FAST_SIM
///////////////////////////////////////////////////////////////////////////////
// local
static ht_data_t ht_data;
static uint8_t frame_period = 10;
static uint8_t sync_len = 200;
static volatile bool calibrating = false;
static int calibration_count = 0;
static float imu_rotation[3] = {0.0 * DEG_TO_RAD, -90.0 * DEG_TO_RAD, (-90.0+22.0) * DEG_TO_RAD};

///////////////////////////////////////////////////////////////////////////////
//no motion to disable OLED display
void detect_motion(int is_moving)
{
    static uint8_t state = 0;  //0: detecting motion, 1=oled pre off mode, 2= oled off mode
	static int cnt = 0;

    if(state == 0) { //in moving 
        if(is_moving) 
            cnt = 0;
        else {
            cnt++;
            if(g_setting.image.auto_off != 3) {
            #ifdef FAST_SIM
                if(cnt > (MOVTION_DUR_1MINUTE*(g_setting.image.auto_off+1))) { 
            #else    
                if(cnt > (MOVTION_DUR_1MINUTE*(g_setting.image.auto_off*2+3))) {
            #endif        
                    state = 1;
                    cnt = 0;
                    LOGI("OLED pre-OFF for protection.");
                    OLED_Brightness(0);
                }
            }
        #ifdef FAST_SIM
            LOGI("IDLE %d",cnt);
        #endif
        }    
    }
    else if(state == 1) { //pre -off
    #ifdef FAST_SIM    
        LOGI("PRE OFF %d",cnt);
    #endif    
        if(is_moving) {
            state = 0;
            cnt = 0;
            OLED_Brightness(g_setting.image.oled);
        }
        else {
            cnt++;
            if(cnt == MOVTION_DUR_1MINUTE) { // 1-min
                LOGI("OLED OFF for protection.");
                beep(); 
                
                OLED_ON(0); //Turn off OLED

                if(g_hw_stat.source_mode ==1) 
                    HDZero_Close(); //Turn off RF

                state = 2;
                cnt = 0;
            }
        }
    }
    else { // in stationery 
    #ifdef FAST_SIM
        LOGI("OFF %d",cnt);
    #endif    
        if(is_moving) {
            cnt++;
            if(cnt == 2) {
                state = 0;
                cnt = 0;
                if(g_hw_stat.source_mode ==1) {
                    HDZero_open();
                    uint8_t ch = g_setting.scan.channel - 1;
	                DM6302_SetChannel(ch);
                }
                LOGI("OLED ON from protection.");
                OLED_Brightness(g_setting.image.oled);
                OLED_ON(1); 
            }
        }
        else {
            if(cnt) cnt = 0;
        }
    }
}

void get_imu_data(int bCalcDiff)
{
    static int dec_cnt;
    static struct bmi2_sens_axes_data gyr_last;
    int16_t  dx,dy,dz;
    uint32_t diff;
    int  is_moving;
    
    get_bmi270(&ht_data.sensor_data);

    dec_cnt++;
    if(dec_cnt != 10) return;  //calibrate dec_cnt to make sure the following code runs at 1Hz
    dec_cnt = 0;

    if(bCalcDiff) {
        dx = ht_data.sensor_data.gyr.x - gyr_last.x;
        dy = ht_data.sensor_data.gyr.y - gyr_last.y;
        dz = ht_data.sensor_data.gyr.z - gyr_last.z;
        diff = dx*dx+dy*dy+dz*dz;
        is_moving = (diff > MOVTION_GYRO_THR) | g_key;
        
        g_key = 0;
        gyr_last = ht_data.sensor_data.gyr;    
        
        //if(is_moving)
        //    LOGI("IMU: %d",diff);
       
        detect_motion(is_moving);
    }
}

static void thread_imu(union sigval timer_data)
{
    get_imu_data(true);
    calc_ht();
}

/////////////////////////////////////////////////////////////////////////////////
//HT function
void init_ht()
{
    ht_data.tiltAngle = 0;      
    ht_data.rollAngle = 0;       
    ht_data.panAngle = 0;       

    ht_data.tiltInverse = 1; 
    ht_data.rollInverse = -1; 
    ht_data.panInverse = -1; 

    ht_data.tiltMaxPulse = 500;
    ht_data.tiltMinPulse = -500; 
    ht_data.tiltCenter = 1500; 
    ht_data.panMaxPulse = 500;
    ht_data.panMinPulse = -500; 
    ht_data.panCenter = 1500; 
    ht_data.rollMaxPulse = 500;
    ht_data.rollMinPulse = -500; 
    ht_data.rollCenter = 1500; 

    ht_data.htChannels[0] = 0;
    ht_data.htChannels[1] = 0;
    ht_data.htChannels[2] = 0;
    
    ht_data.enable = 0;
    set_maxangle_ht(g_setting.ht.max_angle);
    ht_data.acc_offset[0] = g_setting.ht.acc_x;
    ht_data.acc_offset[1] = g_setting.ht.acc_y;
    ht_data.acc_offset[2] = g_setting.ht.acc_z;
    ht_data.gyr_offset[0] = g_setting.ht.gyr_x;
    ht_data.gyr_offset[1] = g_setting.ht.gyr_y;
    ht_data.gyr_offset[2] = g_setting.ht.gyr_z;

    // start timer
    timer_t timerId = 0;
    struct sigevent sev = { 0 };
    struct itimerspec its = {   .it_value.tv_sec  = 1,
                                .it_value.tv_nsec = 0,
                                .it_interval.tv_sec  = 0,
                                .it_interval.tv_nsec = 10000000
                            };

    sev.sigev_notify = SIGEV_THREAD;
    sev.sigev_notify_function = &thread_imu;

    int res = timer_create(CLOCK_REALTIME, &sev, &timerId);
    if (res != 0){
        LOGE("Error timer_create: %s\n", strerror(errno));
        return;
    }

    res = timer_settime(timerId, 0, &its, NULL);
    if (res != 0){
        LOGE("Error timer_settime: %s\n", strerror(errno));
    }
}

void set_maxangle_ht(int angle)
{
    ht_data.tiltFactor = 1000.0 / angle;
    ht_data.rollFactor = 1000.0 / angle;
    ht_data.panFactor = 1000.0 / angle;
}

// Rotate, in Order X -> Y -> Z
static void rotate(float pn[3], const float rot[3])
{
  float out[3];

  // X Rotation
  if (rot[0] != 0) {
    out[0] = pn[0] * 1 + pn[1] * 0 + pn[2] * 0;
    out[1] = pn[0] * 0 + pn[1] * cos(rot[0]) - pn[2] * sin(rot[0]);
    out[2] = pn[0] * 0 + pn[1] * sin(rot[0]) + pn[2] * cos(rot[0]);
    memcpy(pn, out, sizeof(out[0]) * 3);
  }

  // Y Rotation
  if (rot[1] != 0) {
    out[0] = pn[0] * cos(rot[1]) - pn[1] * 0 + pn[2] * sin(rot[1]);
    out[1] = pn[0] * 0 + pn[1] * 1 + pn[2] * 0;
    out[2] = -pn[0] * sin(rot[1]) + pn[1] * 0 + pn[2] * cos(rot[1]);
    memcpy(pn, out, sizeof(out[0]) * 3);
  }

  // Z Rotation
  if (rot[2] != 0) {
    out[0] = pn[0] * cos(rot[2]) - pn[1] * sin(rot[2]) + pn[2] * 0;
    out[1] = pn[0] * sin(rot[2]) + pn[1] * cos(rot[2]) + pn[2] * 0;
    out[2] = pn[0] * 0 + pn[1] * 0 + pn[2] * 1;
    memcpy(pn, out, sizeof(out[0]) * 3);
  }
}

static void calc_gyr(float* gyrAngle) //in degree
{
    gyrAngle[0] = gyr_to_dps(ht_data.sensor_data.gyr.x - ht_data.gyr_offset[0]);
    gyrAngle[1] = gyr_to_dps(ht_data.sensor_data.gyr.y - ht_data.gyr_offset[1]);
    gyrAngle[2] = gyr_to_dps(ht_data.sensor_data.gyr.z - ht_data.gyr_offset[2]);
    rotate(gyrAngle, imu_rotation);
}

static void calc_acc(float* accAngle) //in G
{
    accAngle[0] = acc_to_g(ht_data.sensor_data.acc.x);// - ht_data.acc_offset[0]); 
    accAngle[1] = acc_to_g(ht_data.sensor_data.acc.y);// - ht_data.acc_offset[1]);
    accAngle[2] = acc_to_g(ht_data.sensor_data.acc.z);// - ht_data.acc_offset[2]);
    rotate(accAngle, imu_rotation);
}

// Normalizes any number to an arbitrary range
// by assuming the range wraps around when going below min or above max
float normalize(float value, float start, float end)
{
  float width = end - start;          //
  float offsetValue = value - start;  // value relative to 0

  return (offsetValue - (floor(offsetValue / width) * width)) + start;
  // + start to reset back to start of original range
}

void calibrate_ht()
{
    uint16_t i;
    float accAngle[3];

    LOGI("HT calibration...");
    ht_data.acc_offset[0] = ht_data.acc_offset[1] = ht_data.acc_offset[2] = 0;
    ht_data.gyr_offset[0] = ht_data.gyr_offset[1] = ht_data.gyr_offset[2] = 0;

    calibration_count = 0;
    calibrating = true;
    // Check if finished calibrating
    while(calibrating) {
        usleep(100000);
    }
    ht_data.acc_offset[0] >>= CALIBRATION_BCNT;
    ht_data.acc_offset[1] >>= CALIBRATION_BCNT;
    ht_data.acc_offset[2] >>= CALIBRATION_BCNT;
    ht_data.gyr_offset[0] >>= CALIBRATION_BCNT;
    ht_data.gyr_offset[1] >>= CALIBRATION_BCNT;
    ht_data.gyr_offset[2] >>= CALIBRATION_BCNT;

    ini_putl("ht", "acc_x", ht_data.acc_offset[0], SETTING_INI);
    ini_putl("ht", "acc_y", ht_data.acc_offset[1], SETTING_INI);
    ini_putl("ht", "acc_z", ht_data.acc_offset[2], SETTING_INI);
    ini_putl("ht", "gyr_x", ht_data.gyr_offset[0], SETTING_INI);
    ini_putl("ht", "gyr_y", ht_data.gyr_offset[1], SETTING_INI);
    ini_putl("ht", "gyr_z", ht_data.gyr_offset[2], SETTING_INI);

    LOGI("done!");
}

int calc_ht()
{
    float gyrAngle[3], accAngle[3], tmp;

    if(!calibrating && !ht_data.enable) return 0;

    if (calibrating) {
        ht_data.acc_offset[0] += ht_data.sensor_data.acc.x;
        ht_data.acc_offset[1] += ht_data.sensor_data.acc.y;
        ht_data.acc_offset[2] += ht_data.sensor_data.acc.z;
        ht_data.gyr_offset[0] += ht_data.sensor_data.gyr.x;
        ht_data.gyr_offset[1] += ht_data.sensor_data.gyr.y;
        ht_data.gyr_offset[2] += ht_data.sensor_data.gyr.z;
        calibration_count++;
        if (calibration_count == 1 << CALIBRATION_BCNT)
            calibrating = false;
        return 1;
    }

    calc_gyr(gyrAngle);
    calc_acc(accAngle);
	// LOGI("ACC=%.2f,%.2f,%.2f\tGYR=%.2f,%.2f,%.2f", accAngle[0], accAngle[1], accAngle[2],
    //     gyrAngle[0], gyrAngle[1], gyrAngle[2]);

	MadgwickAHRSupdateIMU(gyrAngle[0] * DEG_TO_RAD, gyrAngle[1] * DEG_TO_RAD, gyrAngle[2] * DEG_TO_RAD, \
                          accAngle[0],              accAngle[1],              accAngle[2]);

	ht_data.panAngle = getYaw() - ht_data.panAngleHome;
	ht_data.tiltAngle = getPitch() - ht_data.tiltAngleHome;
	ht_data.rollAngle = getRoll() - ht_data.rollAngleHome;

    tmp = normalize(ht_data.panAngle, -180.0, 180.0) * ht_data.panInverse * ht_data.panFactor;
    if ((tmp > ht_data.panMinPulse) && (tmp < ht_data.panMaxPulse)) {
        tmp += ht_data.panCenter;
        ht_data.htChannels[0] = (int16_t)tmp;
    }    

    tmp = normalize(ht_data.tiltAngle, -180.0, 180.0) * ht_data.tiltInverse * ht_data.tiltFactor;
    if((tmp > ht_data.tiltMinPulse) && (tmp < ht_data.tiltMaxPulse)) {
        tmp += ht_data.tiltCenter;
        ht_data.htChannels[1] = (int16_t)tmp;
    }

    tmp = normalize(ht_data.rollAngle, -180.0, 180.0) * ht_data.rollInverse * ht_data.rollFactor;
    if((tmp > ht_data.rollMinPulse) && (tmp < ht_data.rollMaxPulse)) {
        tmp += ht_data.rollCenter;
        ht_data.htChannels[2] = (int16_t)tmp;
    }

    Set_HT_dat(ht_data.htChannels[0], ht_data.htChannels[1], ht_data.htChannels[2]);
	return 1;
}

void set_center_position_ht()
{
    ht_data.panAngleHome += ht_data.panAngle;
    ht_data.tiltAngleHome += ht_data.tiltAngle;
    ht_data.rollAngleHome += ht_data.rollAngle;
}

void enable_ht()
{
    ht_data.enable = 1;
    Set_HT_status(ht_data.enable, frame_period, sync_len);
}

void disable_ht()
{
    ht_data.enable = 0;
    Set_HT_status(ht_data.enable, frame_period, sync_len);
}

int16_t* get_ht_channels()
{
    return ht_data.htChannels;
}