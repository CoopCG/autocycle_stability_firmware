//
// Created by Cooper Grill on 3/31/2020.
//

// Arduino libraries
#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <due_can.h>
#include <AccelStepper.h>
//#include <Adafruit_FRAM_SPI.h>

// Internal libraries
#include "IMU.h"
#include "Indicator.h"
#include "StateMachine.h"
#include "StateColors.h"
#include "CANOpen.h"
#include "TorqueMotor.h"
#include "DriveMotor.h"
#include "Controller.h"
#include "PIDController.h"
#include "FSFController.h"
#include "KalmanFilter.h"
#include "BikeModel.h"
#include "ZSS.h"

#include <SPIMemory.h>


// States
#define IDLE    0
#define CALIB   1
#define MANUAL  2
#define ASSIST  3
#define AUTO    4
#define FALLEN  5
#define E_STOP  6

// User request bit flags
#define R_CALIB_MODE        0b00000001U
#define R_MANUAL            0b00000010U
#define R_STOP              0b00000100U
#define R_RESUME            0b00001000U
#define R_TIMEOUT           0b00010000U
#define R_CALIB_GYRO        0b00100000U
#define R_CALIB_ACCEL       0b01000000U
#define R_CALIB_VARIANCE    0b10000000U

// Info for torque motor
#define TM_NODE_ID          127
#define TM_CURRENT_MAX      1000
#define TM_TORQUE_MAX       1000
#define TM_TORQUE_SLOPE     10000   // Thousandths of max torque per second

// State transition constants
#define FTHRESH (PI/4.0)      // Threshold for being fallen over
#define UTHRESH (PI/20.0)     // Threshold for being back upright
#define HIGH_V_THRESH 2.2
#define LOW_V_THRESH 1.8

// Loop timing constants (frequencies in Hz)
#define SPEED_UPDATE_FREQ   5
#define REPORT_UPDATE_FREQ  2
#define STORE_UPDATE_FREQ   50


//#define REQUIRE_ACTUATORS
//#define RADIOCOMM

#ifdef RADIOCOMM

//#include <nRF24L01.h>
#include <RF24.h>

RF24 radio(7, 8);
#define TELEMETRY radio
uint8_t readAddr[] = "UNODE";
uint8_t writeAddr[] = "DNODE";

#else
#define TELEMETRY Serial
#endif

// Device object definitions
IMU imu(2);
Indicator indicator(3, 4, 5, 11);
TorqueMotor *torque_motor;
DriveMotor *drive_motor;
SPIFlash flash(6);

BikeModel bike_model;

KalmanFilter<4, 4, 2> orientation_filter;
KalmanFilter<2, 2, 1> velocity_filter;
KalmanFilter<2, 1, 1> heading_filter;

Controller *controller;


// State variables
uint8_t user_req = 0;       // User request binary flags
uint8_t state = IDLE;
float dt;
float phi = 0.0;            // Roll angle (rad)
float del = 0.0;            // Steering angle (rad)
float dphi = 0.0;           // Roll angle rate (rad/s)
float ddel = 0.0;           // Steering angle rate (rad/s)
float phi_y = 0.0;          // Roll angle measurement (rad)
float del_y = 0.0;          // Steering angle measurement (rad)
float dphi_y = 0.0;         // Roll angle rate measurement (rad/s)
float ddel_y = 0.0;         // Steering angle rate measurement (rad/s)
float v = 0.0;              // Velocity (m/s)
bool free_running = false;

float heading = 0.0;
float dheading = 0.0;

float v_y = 0.0;            // Raw velocity measurement

// Reference variables
float phi_r = 0.0;          // Required roll angle (rad)
float del_r = 0.0;          // Required steering angle (rad)
float v_r = 0.0;            // Required velocity (m/s)

// Control variables
float torque = 0.0;         // Current torque (Nm)

// Filter tuning parameters
float var_drive_motor = 0.04;   // Variance in (m/s^2)^2
float var_roll_accel = 0.64;    // Variance in (rad/s^2)^2
float var_steer_accel = 0.16;   // Variance in (rad/s^2)^2
float var_heading = 0.01;       // Variance in (rad/s^2)^2

const int enPin = A0;       // Information for brake stepper
const int dirPin = A2;
const int stepPin = A1;
AccelStepper stepper(AccelStepper::DRIVER, stepPin, dirPin);

ZSS zss(28, 26, 24, 22, 52, 53);


void report();

void home_delta();

void find_variances(float &var_v, float &var_a, float &var_phi, float &var_del, float &var_dphi, float &var_ddel);

void storeTelemetry();

void retrieveTelemetry();

void clearTelemetry();

void physical_brake(bool engage);

int framAddress = 100;
bool isRecording = false;

#define PARAMETER_ADDR  0x000000
#define TELEMETRY_ADDR  0x001000
#define FLASH_SIZE      2097152
#define SECTOR_SIZE     4096
#define PAGE_SIZE       256

struct StoredParameters {
    float var_v, var_a, var_phi, var_del, var_dphi, var_ddel;
    int16_t ax_off, ay_off, az_off, gx_off, gy_off, gz_off;
} parameters;

struct TelemetryFrame {
    uint8_t state;
    float time, phi, del, dphi, ddel, v, torque, heading, dheading, phi_y, del_y, dphi_y, ddel_y;
} t_frame;
static uint32_t storeAddress = TELEMETRY_ADDR;


void printTelemetryFrame(TelemetryFrame &telemetryFrame);

#define FRAMES_PER_PAGE (PAGE_SIZE / (sizeof t_frame))

struct TelemetyFramePage {
    TelemetryFrame frames[FRAMES_PER_PAGE];
} t_frame_page;

void haltZSS() {
    if (!digitalRead(52) || !digitalRead(53))
        zss.halt();
}

void setup() {
    zss.start();
    Wire.begin();                               // Begin I2C interface
    SPI.begin();                                // Begin Serial Peripheral Interface (SPI)

    Serial.begin(115200);
#ifdef RADIOCOMM
    if (!radio.begin()) {
        Serial.println("Radio not found");
        while (1);
    }
    radio.openWritingPipe(writeAddr);
    radio.openReadingPipe(1, readAddr);
    radio.setPALevel(RF24_PA_HIGH);
    radio.startListening();

#else
    TELEMETRY.begin(115200);                    // Begin Main Serial (UART to USB) communication
#endif


    Serial1.begin(1200);              // Begin Bafang Serial (UART) communication
    Serial2.begin(1200);
    delay(1000);                          // Wait for Serial interfaces to initialize


    Can0.begin(CAN_BPS_1000K);                  // Begin 1M baud rate CAN interface, no enable pin
    Can0.watchFor();                            // Watch for all incoming CAN-Bus messages

    flash.begin();                               // Begin SPI comm to ferroelectric RAM

    analogWriteResolution(12);              // Enable expanded PWM and ADC resolution
    analogReadResolution(12);

    stepper.setAcceleration(1000.0);
    stepper.setMaxSpeed(100.0);

    // Initialize indicator
    indicator.start();
    indicator.beep(100);
    indicator.setPassiveRGB(RGB_STARTUP_P);
    indicator.setBlinkRGB(RGB_STARTUP_B);

#ifdef REQUIRE_ACTUATORS
    // Initialize torque control motor
    torque_motor = new TorqueMotor(&Can0, TM_NODE_ID, TM_CURRENT_MAX, TM_TORQUE_MAX, TM_TORQUE_SLOPE,
                                   8 * PI, 16 * PI, 10);
    torque_motor->start();
    Serial.println("Initialized Torque Motor.");

    Serial.println("Initializing Drive Motor.");
    // Initialize Bafang drive motor
    drive_motor = new DriveMotor(DAC0);
    drive_motor->start();
    Serial.println("Initialized Drive Motor.");
#endif

    imu.start();                                    // Initialize IMU
    imu.configure(2, 2, 2);  // Set accelerometer and gyro resolution, on-chip low-pass filter

    Serial.println("Initializing controller.");
    // Initialize stability controller
    controller = new FSFController(&bike_model, 8.0, -2, -3, -4, -5);

    Serial.println("Initialized controller.");

    Serial.println("Loading parameters from Flash.");
    // Load parameters from Flash
    if (flash.readAnything(PARAMETER_ADDR, parameters)) {
        Serial.println("Loaded parameters from FLash.");
        imu.set_accel_offsets(parameters.ax_off, parameters.ay_off, parameters.az_off);
        imu.set_gyro_offsets(parameters.gx_off, parameters.gy_off, parameters.gz_off);
    } else {
        Serial.print("Failed to load parameters from Flash, error code: 0x");
        Serial.println(flash.error(), HEX);
    }

    //! Temporary sensor covariance overriding parameters
    parameters.var_v = 0.0004;
    parameters.var_a = 0.02;
    parameters.var_phi = 0.00002629879368; // From averaging data
    parameters.var_del = 0.0000001;
    parameters.var_dphi = 0.0000002117716535; // From averaging data
    parameters.var_ddel = 0.0000001;

    Serial.println("Initializing Kalman filter.");
    // Initialize velocity Kalman filter
    velocity_filter.x = {0, 0};                     // Initial state estimate
    velocity_filter.P = BLA::Identity<2, 2>() * 0.1;     // Initial estimate covariance
    velocity_filter.B = {0, 1};                     // Control matrix
    velocity_filter.C = BLA::Identity<2, 2>();      // Sensor matrix
    velocity_filter.R = {                           // Sensor covariance matrix
            parameters.var_v, 0,
            0, parameters.var_a
    };

    // Initialize local orientation Kalman filter
    orientation_filter.x = {0, 0, 0, 0};            // Initial state estimate
    orientation_filter.P = BLA::Identity<4, 4>() * 0.1;      // Initial estimate covariance
    orientation_filter.C = BLA::Identity<4, 4>();   // Sensor matrix
    orientation_filter.R = {                        // Sensor covariance matrix
            parameters.var_phi, 0, 0, 0,
            0, parameters.var_del, 0, 0,
            0, 0, parameters.var_dphi, 0,
            0, 0, 0, parameters.var_ddel
    };

    float var_gyro_z = 0.01;

    // Initialize heading Kalman filter
    heading_filter.x = {0, 0};                      // Initial state estimate
    heading_filter.P = BLA::Identity<2, 2>() * 0.1; // Initial estimate covariance
    heading_filter.B = {0, 1};
    heading_filter.C = {0, 1};                      // Sensor matrix
    heading_filter.R = {                            // Sensor covariance matrix
            var_gyro_z
    };
    Serial.println("Initialized Kalman filter.");

    // Set up ZSS stop pins
    pinMode(52, INPUT_PULLUP);
    pinMode(53, INPUT_PULLUP);
    pinMode(50, OUTPUT);
    pinMode(51, OUTPUT);
    digitalWrite(50, HIGH);
    digitalWrite(51, HIGH);

    Serial.println("Attaching ZSS interrupts.");
    attachInterrupt(digitalPinToInterrupt(52), haltZSS, FALLING);
    attachInterrupt(digitalPinToInterrupt(53), haltZSS, FALLING);
    Serial.println("Attached ZSS interrupts.");

    assert_idle();

    pinMode(enPin, OUTPUT);
    digitalWrite(enPin, LOW);

    Serial.println("Finished setup.");

    delay(1000);
}

void loop() {
    static unsigned long last_time = millis();
    static unsigned long last_speed_time = millis();
    static unsigned long last_report_time = millis();
    static unsigned long last_store_time = millis();
    static unsigned long timeout = 0;
    dt = (float) (millis() - last_time) / 1000.0f;
    last_time = millis();

    // Update sensor information
    imu.update();
#ifdef REQUIRE_ACTUATORS
    torque_motor->update();
#endif

    // Update heading Kalman filter parameters
    heading_filter.A = {
            1, dt,
            0, 1
    };
    heading_filter.Q = {
            var_heading * dt * dt, var_heading * dt,
            var_heading * dt, var_heading
    };
    heading_filter.predict({0});
    heading_filter.update({imu.gyroZ()});
    heading = heading_filter.x(0);
    dheading = heading_filter.x(1);


    // Update velocity Kalman filter parameters
    velocity_filter.A = {
            1, dt,
            0, 1
    };
    velocity_filter.Q = {
            var_drive_motor * dt * dt, var_drive_motor * dt,
            var_drive_motor * dt, var_drive_motor
    };
//    velocity_filter.x(1) = imu.accelX();
    velocity_filter.predict({0});

    // Update velocity state measurement
    if (millis() - last_speed_time >= 1000 / SPEED_UPDATE_FREQ) {
#ifdef REQUIRE_ACTUATORS
        v_y = drive_motor->getSpeed();
#endif
//        v = v_y;
        float a_y = imu.accelX();
        last_speed_time = millis();

        velocity_filter.update({v_y, a_y});
    }
    v = velocity_filter.x(0);

    // Update orientation state measurement
    float g_mag = imu.accelY() * imu.accelY() +
                  imu.accelZ() * imu.accelZ();    // Check if measured orientation gravity vector exceeds feasibility
    phi_y = g_mag <= 10 * 10 ? atan2(imu.accelY(), imu.accelZ()) : phi_y;
    dphi_y = imu.gyroX();
#ifdef REQUIRE_ACTUATORS
    del_y = torque_motor->getPosition();
    ddel_y = torque_motor->getVelocity();
    torque = torque_motor->getTorque();
#endif

    // Update orientation Kalman filter parameters
    orientation_filter.A = bike_model.kalmanTransitionMatrix(v, dt, free_running);
    orientation_filter.B = bike_model.kalmanControlsMatrix(v, dt, free_running);
    orientation_filter.Q = {
            var_roll_accel / 4 * dt * dt * dt * dt, var_roll_accel / 2 * dt * dt * dt, 0, 0,
            var_roll_accel / 2 * dt * dt * dt, var_roll_accel * dt * dt, 0, 0,
            0, 0, var_steer_accel / 4 * dt * dt * dt * dt, var_steer_accel / 2 * dt * dt * dt,
            0, 0, var_steer_accel / 2 * dt * dt * dt, var_steer_accel * dt * dt,
    };

    // Update orientation state estimate
    orientation_filter.predict({0, torque});
    orientation_filter.update({phi_y, del_y, dphi_y, ddel_y});
    phi = orientation_filter.x(0);
    del = orientation_filter.x(1);
    dphi = orientation_filter.x(2);
    ddel = orientation_filter.x(3);

    //!!!! DEBUG ONLY !!!!//
    v = 0.0;
//    phi = 0;

    // Update indicator
    indicator.update();


    // Act based on machine state, transition if necessary
    switch (state) {
        case IDLE:      // Idle
            // Transitions
            if (fabs(phi) > FTHRESH)
                assert_fallen();
            if (v > 0.6)
                assert_assist();
            if (user_req & R_CALIB_MODE)
                assert_calibrate();
            if (user_req & R_MANUAL)
                assert_manual();
            if (user_req & R_STOP)
                assert_emergency_stop();

            // Action
            idle();

            break;

        case CALIB:     // Sensor calibration
            // Transitions
            if (user_req & R_RESUME) {
                user_req &= ~R_CALIB_MODE;
                assert_idle();
            }

            // Action
            calibrate();


            break;

        case MANUAL:    // Manual operation
            // Transitions
            if (user_req & R_RESUME) {
                user_req &= ~(R_RESUME | R_MANUAL);
                assert_idle();
            }

            // Action
            manual();

            break;

        case ASSIST:    // Assisted (training wheel) motion, only control heading
            // Transitions
            if (fabs(phi) > FTHRESH)
                assert_fallen();
            if (v > HIGH_V_THRESH)
                assert_automatic();
            if (v < 0.5)
                assert_idle();
            if (user_req & R_STOP)
                assert_emergency_stop();

            // Action
            assist();

            break;

        case AUTO:      // Automatic motion, control heading and stability
            // Transitions
            if (fabs(phi) > FTHRESH)
                assert_fallen();
            if (v < LOW_V_THRESH)
                assert_assist();
            if (user_req & R_STOP)
                assert_emergency_stop();

            // Action
            automatic();

            break;

        case FALLEN:    // Fallen
            // Transitions
            if (fabs(phi) < UTHRESH)
                assert_idle();

            // Action
            fallen();
            break;

        case E_STOP:    // Emergency stop
            // Transitions
            if (fabs(phi) > FTHRESH)
                assert_fallen();
            if (user_req & R_RESUME) {
                user_req &= ~(R_RESUME | R_STOP);
                assert_idle();
            }

            // Action
            emergency_stop();
            break;

        default:        // Invalid state, fatal error
            indicator.setPassiveRGB(0, 0, 0);
            break;
    }

    if ((user_req & R_TIMEOUT) && millis() > timeout) {
        user_req &= ~R_TIMEOUT;
        v_r = 0;
#ifdef REQUIRE_ACTUATORS
        drive_motor->setSpeed(0);
#endif
        Serial.println("Timed out!");
    }

    // Report state, reference, and control values
    if (millis() - last_report_time >= 1000 / REPORT_UPDATE_FREQ) {
        report();
        last_report_time = millis();
    }
    if (millis() - last_store_time >= 1000 / STORE_UPDATE_FREQ && isRecording) {
        storeTelemetry();
        last_store_time = millis();
    }

    if (TELEMETRY.available()) {
        delay(100);
#ifdef RADIOCOMM
        uint8_t buffer[32];
        TELEMETRY.read(buffer, 32);
        uint8_t c = buffer[0];
        Serial.print(c);
        Serial.println(buffer[1]);


        switch (c) {
            case 's':
                v_r = *((float *) &(buffer[2]));
                drive_motor->setSpeed(v_r);
                break;
            case 'd':
                del_r = *((float *) &(buffer[2]));
                break;
            case 'c':
                user_req = buffer[2];
                break;
            case 't':
                v_r = *((float *) &(buffer[2]));
                drive_motor->setSpeed(v_r);
                timeout = millis() + *((uint32_t *) &(buffer[6]));
                user_req |= R_TIMEOUT;
                isRecording = true;
                break;
            case 'h':
                assert_idle();
                home_delta();
                delay(250);
                assert_idle();
                break;
            case 'r':
                isRecording = true;
                break;
            case 'q':
                isRecording = false;
                break;

            default:
                break;
        }
#else
        uint8_t c = TELEMETRY.read();
        delay(20);

        switch (c) {
            case 's':
                v_r = Serial.parseFloat();
#ifdef REQUIRE_ACTUATORS
                drive_motor->setSpeed(v_r);
#endif
                break;
            case 'd':
                del_r = Serial.parseFloat();
                break;
            case 'c':
                user_req |= Serial.parseInt();
                break;
            case 't':
                v_r = Serial.parseFloat();
                timeout = millis() + Serial.parseInt();
                user_req |= R_TIMEOUT;
                break;
            case 'f':
                retrieveTelemetry();
                break;
            case 'r':
                isRecording = true;
                break;
            case 'q':
                isRecording = false;
                break;
            case 'z':
                clearTelemetry();
                break;

            default:
                break;
        }
        while (Serial.available() > 0)
            Serial.read();
#endif
        indicator.boop(100);
    }

    if (Serial.available()) {
        if (Serial.read() == 'f') {
            retrieveTelemetry();
        }
    }
}

void idle() {

}

void calibrate() {
    bool calibrated = false;

    if (user_req & R_CALIB_GYRO) {
        if (imu.calibrateGyroBias()) {
            indicator.beepstring((uint8_t) 0b11101110);

            imu.get_gyro_offsets(parameters.gx_off, parameters.gy_off, parameters.gz_off);
            calibrated = true;
        } else {
            indicator.beepstring((uint8_t) 0b10001000);
        }

        user_req = user_req & ~R_CALIB_GYRO;
    }

    if (user_req & R_CALIB_ACCEL) {
        if (imu.calibrateAccelBias(0, 0, GRAV)) {
            indicator.beepstring((uint8_t) 0b10101010);

            imu.get_accel_offsets(parameters.ax_off, parameters.ay_off, parameters.az_off);
            calibrated = true;
        } else {
            indicator.beepstring((uint8_t) 0b00110011);
        }

        user_req = user_req & ~R_CALIB_ACCEL;
    }

    if (user_req & R_CALIB_VARIANCE) {
        delay(100);

        find_variances(parameters.var_v, parameters.var_a, parameters.var_phi, parameters.var_del,
                       parameters.var_dphi, parameters.var_ddel);
        velocity_filter.R = {
                parameters.var_v, 0,
                0, parameters.var_a
        };
        orientation_filter.R = {
                parameters.var_phi, 0, 0, 0,
                0, parameters.var_del, 0, 0,
                0, 0, parameters.var_dphi, 0,
                0, 0, 0, parameters.var_ddel
        };

        calibrated = true;
        user_req = user_req & ~R_CALIB_VARIANCE;
    }

    if (calibrated) {
        if (flash.eraseSector(PARAMETER_ADDR)) {
            if (flash.writeAnything(PARAMETER_ADDR, parameters)) {
                Serial.println("Recorded new parameters.");
            } else {
                Serial.print("Failed to record new calibration parameters, error: 0x");
                Serial.println(flash.error(), HEX);
            }
        } else {
            Serial.print("Failed to erase parameter sector, error: ");
            Serial.println(flash.error(), HEX);
        }
    }
}


void storeTelemetry() {
    static int i = 0;

    if (storeAddress + PAGE_SIZE < FLASH_SIZE) {
        t_frame_page.frames[i].state = state;
        t_frame_page.frames[i].time = (float) millis() / 1000.0f;
        t_frame_page.frames[i].phi = phi;
        t_frame_page.frames[i].del = del;
        t_frame_page.frames[i].dphi = dphi;
        t_frame_page.frames[i].ddel = ddel;
        t_frame_page.frames[i].v = v;
        t_frame_page.frames[i].torque = torque;
        t_frame_page.frames[i].heading = heading;
        t_frame_page.frames[i].dheading = dheading;
        t_frame_page.frames[i].phi_y = phi_y;
        t_frame_page.frames[i].del_y = del_y;
        t_frame_page.frames[i].dphi_y = dphi_y;
        t_frame_page.frames[i].ddel_y = ddel_y;

        i++;

        if (i >= FRAMES_PER_PAGE) {
            flash.writeAnything(storeAddress, t_frame_page);
            storeAddress += PAGE_SIZE;
            i = 0;
        }
    }
}


void retrieveTelemetry() {
    Serial.println();
    Serial.println();
    Serial.println("RETRIEVAL BEGINNING");
    uint32_t address = TELEMETRY_ADDR;

    while (address + PAGE_SIZE < FLASH_SIZE) {
        flash.readAnything(address, t_frame_page);

        address += PAGE_SIZE;
        if (address % SECTOR_SIZE == 0)
            flash.eraseSector(address - SECTOR_SIZE);

        // Escape if we have hit the end of recorded data
        if (t_frame_page.frames[0].state == 255)
            break;

        for (auto &frame : t_frame_page.frames) {
            printTelemetryFrame(frame);
            delay(1);
        }
    }

    Serial.println("RETRIEVAL ENDED");
    Serial.println();
    Serial.println();
    storeAddress = TELEMETRY_ADDR;
}

void clearTelemetry() {
    for (int i = TELEMETRY_ADDR; i < FLASH_SIZE; i += SECTOR_SIZE) {
        if (flash.eraseSector(i)) {
            Serial.print("Erased sector at ");
            Serial.println(i, HEX);
        } else {
            Serial.print("Failed to erase sector at ");
            Serial.println(i, HEX);
        }
    }
}

void printTelemetryFrame(TelemetryFrame &telemetryFrame) {
    Serial.print(telemetryFrame.state);
    Serial.print('\t');
    Serial.print(telemetryFrame.time, 4);
    Serial.print('\t');
    Serial.print(telemetryFrame.phi, 4);
    Serial.print('\t');
    Serial.print(telemetryFrame.del, 4);
    Serial.print('\t');
    Serial.print(telemetryFrame.dphi, 4);
    Serial.print('\t');
    Serial.print(telemetryFrame.ddel, 4);
    Serial.print('\t');
    Serial.print(telemetryFrame.v, 4);
    Serial.print('\t');
    Serial.print(telemetryFrame.torque, 4);
    Serial.print('\t');
    Serial.print(telemetryFrame.heading, 4);
    Serial.print('\t');
    Serial.print(telemetryFrame.dheading, 4);
    Serial.print('\t');
    Serial.print(telemetryFrame.phi_y, 4);
    Serial.print('\t');
    Serial.print(telemetryFrame.del_y, 4);
    Serial.print('\t');
    Serial.print(telemetryFrame.dphi_y, 4);
    Serial.print('\t');
    Serial.print(telemetryFrame.ddel_y, 4);
    Serial.println();
}


void manual() {

}

void assist() {
    float er = del_r;
#ifdef REQUIRE_ACTUATORS
    torque_motor->setPosition(er);
#endif
}

void automatic() {
    float u = controller->control(phi, del, dphi, ddel, phi_r, del_r, v, dt);
#ifdef REQUIRE_ACTUATORS
    torque_motor->setTorque(u);
#endif
}

void fallen() {

}

void emergency_stop() {

}

// State assertion
void assert_idle() {
    state = IDLE;
    free_running = false;
    physical_brake(false);
    zss.deploy();
#ifdef REQUIRE_ACTUATORS
    while (!torque_motor->shutdown());
#endif

    indicator.disablePulse();
    indicator.setPassiveRGB(RGB_IDLE_P);
    indicator.setBlinkRGB(RGB_IDLE_B);
}

void assert_calibrate() {
    state = CALIB;
    free_running = false;

    indicator.setPassiveRGB(RGB_CALIB_P);
    indicator.setBlinkRGB(RGB_CALIB_B);
    indicator.setPulse(250, 250);
}

void assert_manual() {
    state = MANUAL;
    free_running = false;

    indicator.disablePulse();
    indicator.setPassiveRGB(RGB_MANUAL_P);
    indicator.setBlinkRGB(RGB_MANUAL_B);
}

void assert_assist() {
    state = ASSIST;
    free_running = false;
    zss.deploy();
#ifdef REQUIRE_ACTUATORS
    torque_motor->setMode(OP_PROFILE_POSITION);
    while (!torque_motor->enableOperation());
#endif

    indicator.disablePulse();
    indicator.setPassiveRGB(RGB_ASSIST_P);
    indicator.setBlinkRGB(RGB_ASSIST_B);
}

void assert_automatic() {
    state = AUTO;
    free_running = true;
    zss.retract();
#ifdef REQUIRE_ACTUATORS
    torque_motor->setMode(OP_PROFILE_TORQUE);
    while (!torque_motor->enableOperation());
#endif

    indicator.disablePulse();
    indicator.setPassiveRGB(RGB_AUTO_P);
    indicator.setBlinkRGB(RGB_AUTO_B);
    indicator.beep();
}

void assert_fallen() {
    state = FALLEN;
    free_running = false;
#ifdef REQUIRE_ACTUATORS
    drive_motor->setSpeed(0);
    while (!torque_motor->shutdown());
#endif

    indicator.setPassiveRGB(RGB_FALLEN_P);
    indicator.setBlinkRGB(RGB_FALLEN_B);
    indicator.setPulse(500, 1500);
}

void assert_emergency_stop() {
    state = E_STOP;
    free_running = false;
    physical_brake(true);
    zss.deploy();
#ifdef REQUIRE_ACTUATORS
    drive_motor->setSpeed(0);
    while (!torque_motor->shutdown());
#endif

    indicator.disablePulse();
    indicator.setPassiveRGB(RGB_E_STOP_P);
    indicator.setBlinkRGB(RGB_E_STOP_B);
}

uint8_t checksum(const uint8_t *buffer, int len) {
    unsigned int acc = 0;

    for (int i = 0; i < len; i++)
        acc += buffer[i];

    return acc % 256;
}

void report() {
#ifdef RADIOCOMM
    TELEMETRY.stopListening();
    delay(20);
    uint8_t frame[32];

    frame[0] = 13;          // State telemetry frame header
    frame[1] = sizeof frame;

    *((float *) &(frame[2])) = state;
    *((float *) &(frame[6])) = phi;
    *((float *) &(frame[10])) = del;
    *((float *) &(frame[14])) = dphi;
    *((float *) &(frame[18])) = ddel;
    *((float *) &(frame[22])) = torque;
    *((float *) &(frame[26])) = v;
    frame[30] = checksum(frame, 30);
    frame[31] = 0;

    TELEMETRY.write(frame, 32);
    delay(20);


//    frame[0] = 14;          // Setpoint telemetry frame header
//    frame[1] = sizeof frame;
//
//    *((float *) &(frame[2])) = 0;
//    *((float *) &(frame[6])) = phi_r;
//    *((float *) &(frame[10])) = del_r;
//    *((float *) &(frame[14])) = v_r;
//    *((float *) &(frame[18])) = heading;
//    *((float *) &(frame[22])) = dheading;
//    *((float *) &(frame[26])) = millis() / 1000.0f;
//    frame[30] = checksum(frame, 30);
//    frame[31] = 0;
//
//
//    TELEMETRY.write(frame, 32);
//    delay(20);


//    frame[0] = 15;          // Raw sensor telemetry frame header
//    frame[1] = sizeof frame;
//
//    *((float *) &(frame[2])) = imu.accelX();
//    *((float *) &(frame[6])) = imu.accelY();
//    *((float *) &(frame[10])) = imu.accelZ();
//    *((float *) &(frame[14])) = imu.gyroX();
//    *((float *) &(frame[18])) = v_y;
//    *((float *) &(frame[22])) = torque_motor->getPosition();
//    *((float *) &(frame[26])) = torque_motor->getVelocity();
//    frame[30] = checksum(frame, 30);
//    frame[31] = 0;
//
//
//    TELEMETRY.write(frame, 32);
//    delay(20);

    TELEMETRY.startListening();
#else
    Serial.print(state);
    Serial.print('\t');
    Serial.print(phi, 4);
    Serial.print('\t');
    Serial.print(del, 4);
    Serial.print('\t');
    Serial.print(dphi, 4);
    Serial.print('\t');
    Serial.print(ddel, 4);
    Serial.print('\t');
    Serial.print(v, 4);
    Serial.print('\t');
    Serial.print(torque, 4);
    Serial.print('\t');
    Serial.print(heading, 4);
    Serial.print('\t');
    Serial.print(dheading, 4);
    Serial.print('\t');
    Serial.print((float) millis() / 1000.0f, 4);

    Serial.print('\t');
    Serial.print(imu.accelX(), 4);
    Serial.print('\t');
    Serial.print(imu.accelY(), 4);
    Serial.print('\t');
    Serial.print(imu.accelZ(), 4);
    Serial.print('\t');
    Serial.print(imu.gyroX(), 4);
    Serial.print('\t');
    Serial.print(imu.gyroY(), 4);
    Serial.print('\t');
    Serial.print(imu.gyroZ(), 4);

    Serial.println();
    Serial.flush();
#endif

}

void find_variances(float &var_v, float &var_a, float &var_phi, float &var_del, float &var_dphi, float &var_ddel) {
    float data[CALIB_SAMP][6];
    float v_acc, a_acc, phi_acc, del_acc, dphi_acc, ddel_acc;
    v_acc = a_acc = phi_acc = del_acc = dphi_acc = ddel_acc = 0;

    for (auto &i : data) {
        i[0] = drive_motor->getSpeed();
        i[1] = imu.accelX();
        i[2] = atan2(imu.accelY(), imu.accelZ());
        i[3] = torque_motor->getPosition();
        i[4] = imu.gyroX();
        i[5] = torque_motor->getVelocity();

        v_acc += i[0];
        a_acc += i[1];
        phi_acc += i[2];
        del_acc += i[3];
        dphi_acc += i[4];
        ddel_acc += i[5];

        delay(20);
    }

    float v_mean = v_acc / CALIB_SAMP;
    float a_mean = a_acc / CALIB_SAMP;
    float phi_mean = phi_acc / CALIB_SAMP;
    float del_mean = del_acc / CALIB_SAMP;
    float dphi_mean = dphi_acc / CALIB_SAMP;
    float ddel_mean = ddel_acc / CALIB_SAMP;

    float v_var_acc, a_var_acc, phi_var_acc, del_var_acc, dphi_var_acc, ddel_var_acc;
    v_var_acc = a_var_acc = phi_var_acc = del_var_acc = dphi_var_acc = ddel_var_acc = 0;

    for (auto &i : data) {
        v_var_acc += (i[0] - v_mean) * (i[0] - v_mean);
        a_var_acc += (i[1] - a_mean) * (i[1] - a_mean);
        phi_var_acc += (i[2] - phi_mean) * (i[2] - phi_mean);
        del_var_acc += (i[3] - del_mean) * (i[3] - del_mean);
        dphi_var_acc += (i[4] - dphi_mean) * (i[4] - dphi_mean);
        ddel_var_acc += (i[5] - ddel_mean) * (i[5] - ddel_mean);
    }

    var_v = v_var_acc / CALIB_SAMP;
    var_a = a_var_acc / CALIB_SAMP;
    var_phi = phi_var_acc / CALIB_SAMP;
    var_del = del_var_acc / CALIB_SAMP;
    var_dphi = dphi_var_acc / CALIB_SAMP;
    var_ddel = ddel_var_acc / CALIB_SAMP;
}

void home_delta() {
#ifdef REQUIRE_ACTUATORS
    torque_motor->calibrate();
    Serial.println("Successfully calibrated torque motor.");
    delay(1000);
    torque_motor->setMode(OP_PROFILE_POSITION);
    while (!torque_motor->enableOperation());
    torque_motor->setPosition(0);
    Serial.println("Succesfully reset to zero position.");
    delay(3000);
#endif
}

void physical_brake(bool engage) {
    int steps = floor(70 / (20 * 3.1415) * 200);

    if (engage) {
        stepper.moveTo(steps);
    } else {
        stepper.moveTo(0);
    }

    stepper.runToPosition();
}
