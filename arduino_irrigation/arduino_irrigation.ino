// Libraries needed: ArduinoJson, Adafruit_ADS1015, Adafruit_DHT (Temperature and humidty sensor)
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_ADS1015.h>
#include <DHT.h>

/*
 * Modes:
 *   0  just controlling the valve
 *   1  controlling the valve based on flow limit
 *
 * JSON format: (<0-x> means an integer range)
 *   Input:
 *      {
 *          "mode": <0-1>,
 *          "targetValvePos0": <0-100>,
 *          "targetValvePos1": <0-100>,
 *          "flowLimit": <0-x>,
 *          "reportInterval": <1000-x>  // Under 1000 will reset the interval to C_REPORT_INTERVAL
 *      }
 *   Output:
 *      {
 *          "timeStamp": <0-x>,
 *          "soilHumidity": [
 *              <0-x>,
 *              <0-x>,
 *              ...
 *          ],
 *          "flowRate": [
 *              <0-x>,
 *              <0-x>
 *          ],
 *          "airTemperature": <0-x>,
 *          "enclosureTemperature": <0-x>,
 *          "airHumidity": <0-x>,
 *          "doorOpen": <true or false>,
 *          "targetValvePos": <0-x>,
 *
 *              // Also report back the current input values
 *          "mode": <0-1>,
 *          "targetValvePos": <0-255>,
 *          "flowLimit": <0-x>,
 *      }
 *
 * TODO:
 *      get sensor data
 *      Maybe report an average soilHumidity value (To make automation easier)
 *
 * IDEAS:
 *      Maybe implement a RTC for an internet-less clock
 */


// DEFINITIONS //

    // Define output pins for the valve
#define PIN_VALVE_MOTOR_A0  8
#define PIN_VALVE_MOTOR_A1  9
#define PIN_VALVE_MOTOR_B0  10
#define PIN_VALVE_MOTOR_B1  11
    // Define input pins for the valve state
#define PIN_VALVE_OPEN_0    4
#define PIN_VALVE_CLOSE_0   5
#define PIN_VALVE_OPEN_1    6
#define PIN_VALVE_CLOSE_1   7
    // Define input pin for the flow meter sensor (Must be an interrupt)
#define PIN_FLOW_METER_0    2
#define PIN_FLOW_METER_1    3
    // Define output pin for a simple status LED
#define PIN_STATUS          13
    // Define Other sensors
#define PIN_DOOR            A3
#define PIN_TEMP_INT        A0
#define PIN_TEMP_EXT        A2
    // Define I2C pins
#define PIN_SDA             A4
#define PIN_SCL             A5

    // Define Constants
#define C_REPORT_INTERVAL   4000    // The default amount of time (ms) between sending sensor data via serial
#define C_VALVE_OPEN_TIME   4100    // The amount of time (ms) it takes to open the valve
#define C_VALVE_CLOSE_TIME  4100    // The amount of time (ms) it takes to close the valve
#define C_BAUD_RATE         9600    // Sets the serial Baud rate
#define C_ADC_BASE_ADDR     0x48    // Sets the base I2C address of the ADS1115
#define C_MOIST_SENS_AMOUNT 16      // Sets the amount of moisture sensors connected via the external ADC's
#define C_FLOW_SENS_AMOUNT  2       // Sets the amount of flow sensors connected
#define C_MAX_FLOW_RATE     35      // FLOW SENSOR Maximum flow rate (in L/min)
#define C_FLOW_CALIBRATION  7.71    // FLOW SENSOR Pulses per second per litre/minute of flow. (Pulses per liter / 60)
#define C_DHT_TYPE          DHT22   // Set DHT (Temperature and Humidity sensor) type

    // Define Maximum Array Sizes
#define C_SIZE_INPUT        100
#define C_SIZE_MSG          100
#define C_SIZE_OUTPUT       400

    // Define helper functions
#define ARRAYSIZE(x)        (sizeof(x)/sizeof(*x))         // define a macro to calculate array size
#define SOILCALIBRATION(x)  (map(x, 15000, 9000, 0, 100))  // this function is used to calibrate the soil sensors


// GLOBAL VARIABLES //

    // Input (From Serial)
uint8_t targetValvePos0;
uint8_t targetValvePos1;
uint16_t reportInterval = C_REPORT_INTERVAL;

    // Output (To Serial)
uint16_t soilHumidity[C_MOIST_SENS_AMOUNT];
uint16_t avgSoilHumidity;
uint16_t airHumidity;
float flowRate[2];         // Currently picked up flow rate in L/min
float airTemperature;
float enclosureTemperature; // Temperature inside the enclosure
bool doorOpen;

    // Don't change these values outside their intended functions!
long targetValvePosMsec0;
long targetValvePosMsec1;
unsigned long lastCheckTime;
unsigned long lastTime;
int reportDelta = 0;
int delta = 0;
int motorState0; // 0 = stop, -1 = closing, 1 = opening
int motorState1;

volatile byte pulseCount0;
volatile byte pulseCount1;


// CLASS DEFINITIONS //

Adafruit_ADS1115 Adc0(C_ADC_BASE_ADDR);
Adafruit_ADS1115 Adc1(C_ADC_BASE_ADDR + 1);
Adafruit_ADS1115 Adc2(C_ADC_BASE_ADDR + 2);
Adafruit_ADS1115 Adc3(C_ADC_BASE_ADDR + 3);

DHT Dht0(PIN_TEMP_EXT, C_DHT_TYPE);
DHT Dht1(PIN_TEMP_INT, C_DHT_TYPE);


// TOP FUNCTIONS //

void setup() {
    // Initialize serial port
    Serial.begin(C_BAUD_RATE);
    while (!Serial) continue;

    // Initialize pins
    pinMode(PIN_STATUS, OUTPUT);
    pinMode(PIN_FLOW_METER_0, INPUT);
    pinMode(PIN_FLOW_METER_1, INPUT);
    pinMode(PIN_DOOR, INPUT_PULLUP);

    pinMode(PIN_VALVE_OPEN_0, INPUT_PULLUP);
    pinMode(PIN_VALVE_CLOSE_0, INPUT_PULLUP);
    pinMode(PIN_VALVE_OPEN_1, INPUT_PULLUP);
    pinMode(PIN_VALVE_CLOSE_1, INPUT_PULLUP);

    pinMode(PIN_VALVE_MOTOR_A0, OUTPUT);
    pinMode(PIN_VALVE_MOTOR_A1, OUTPUT);
    pinMode(PIN_VALVE_MOTOR_B0, OUTPUT);
    pinMode(PIN_VALVE_MOTOR_B1, OUTPUT);

    // Initialize ADCs
    Adc0.begin();
    Adc1.begin();
    Adc2.begin();
    Adc3.begin();

    // Initialize DHTs
    Dht0.begin();
    Dht1.begin();

    // Attach interrupts for the water flow sensors
    setInterrupts(true);
}

void loop() {
    recieveStates();

    // Perform functions at set intervals
    unsigned long currentCheckTime = millis();
    delta = millis() - lastTime;
    lastTime = currentCheckTime;
    reportDelta = currentCheckTime - lastCheckTime;
    if (currentCheckTime - lastCheckTime >= reportInterval) {
        lastCheckTime = currentCheckTime;

        // Timout reached, Perform these functions below
        updateSensorValues();
        sendStates();
    }

    checkValves();
}


// EXTRA FUNCTIONS //

// Write Sensor data to the Serial port
void sendStates() {
    StaticJsonDocument<C_SIZE_OUTPUT> jbuf;

    // Add time stamp, to make sure the output is unique
    jbuf["timeStamp"] = millis();

    // Build subarray containing the soil humidity values
    JsonArray arr0 = jbuf.createNestedArray("soilHumidity");
    for (int i = 0; i < C_MOIST_SENS_AMOUNT; i++) {
        arr0.add(soilHumidity[i]);
    }

    // Build subarray containing the flow rate values
    JsonArray arr1 = jbuf.createNestedArray("flowRate");
    for (int i = 0; i < C_FLOW_SENS_AMOUNT; i++) {
        arr1.add(flowRate[i]);
    }

    // Build subarray containing the target valve positions
    JsonArray arr2 = jbuf.createNestedArray("targetValvePos");
    arr2.add(targetValvePos0);
    arr2.add(targetValvePos1);

    // TODO: Maybe add current valve positions

    // Add additional values
    jbuf["airHumidity"] = airHumidity;
    jbuf["airTemperature"] = airTemperature;
    jbuf["enclosureTemperature"] = enclosureTemperature;
    jbuf["doorOpen"] = doorOpen;
    jbuf["avgSoilHumidity"] = avgSoilHumidity;

    // Output data to serial
    serializeJson(jbuf, Serial);

    // Make sure to print a new line
    Serial.println();
    Serial.flush();
}


// Read Serial data and parse values
void recieveStates() {
    if (Serial.available()) {

        StaticJsonDocument<C_SIZE_INPUT> jsonInput;
        DeserializationError err = deserializeJson(jsonInput, Serial);

        // Test if parsing succeeds.
        if (!err == DeserializationError::Ok) {
            printError(err.c_str());
            return;
        }

        // Parsing succeeds! Set variables.
        int valvePos0 = getJsonKeyValueAsInt(jsonInput, "targetValvePos0", targetValvePos0);
        int valvePos1 = getJsonKeyValueAsInt(jsonInput, "targetValvePos1", targetValvePos1);
        reportInterval = getJsonKeyValueAsInt(jsonInput, "reportInterval", reportInterval);
        reportInterval = (reportInterval >= 1000) ? reportInterval : C_REPORT_INTERVAL;

        // Move valves if requested
        if (valvePos0 != targetValvePos0)
            setValve(0, valvePos0);
        if (valvePos1 != targetValvePos1)
            setValve(1, valvePos1);

        // Wait until the remaining data has been recieved and then make sure the serial buffer is empty.
        delay(10);
        while (Serial.available() > 0) {
            char t = Serial.read();
        }
    }
}

// Read all sensors and update global values
void updateSensorValues(){
    updateSoilHumidity();
    updateFlowRate();
    updateTemperatureHumidity();

    doorOpen = digitalRead(PIN_DOOR);
}

// Turn the valve until it (predictivly) reaches the desired state. (0-100)
void setValve(byte which, uint8_t percentage) {
    int turnLength;
    int direction;
    unsigned long * selectedMsec;
    uint8_t * selectedPerc;

    // Set references to the global variables
    switch(which) {
    case 0:
        selectedMsec = &targetValvePosMsec0;
        selectedPerc = &targetValvePos0;
        break;
    case 1:
        selectedMsec = &targetValvePosMsec1;
        selectedPerc = &targetValvePos1;
        break;
    }

    // Set the direction which the valve should move to
    if (percentage > *selectedPerc)
        direction = 1;
    else if (percentage < *selectedPerc)
        direction = -1;
    else
        direction = 0;

    // Get the opening or closing time, depending on the direction
    turnLength = (direction == -1) ? C_VALVE_CLOSE_TIME : C_VALVE_OPEN_TIME;
    int percentageChange = abs(*selectedPerc - percentage);

    // Determine for how long the valve should turn
    switch(percentage) {
        case 0:
            *selectedMsec = C_VALVE_CLOSE_TIME + 2000; // Add some time to ensure proper sealing
            break;
        case 100:
            *selectedMsec = C_VALVE_OPEN_TIME + 2000;
            break;
        default:
            *selectedMsec = (turnLength / 100) * percentageChange;
            break;
    }

    // Update target percentage
    *selectedPerc = percentage;

    // Move the motor in the desired direction
    setMotor(which, direction);
}

// Check wheter a valve should be stopped
void checkValves() {
    // Uses delta
    // Count down currently moving motors, stop them if they have reached their target
    if (targetValvePosMsec0 > 0 && motorState0 != 0) {
        targetValvePosMsec0 -= delta;
    } else if (targetValvePosMsec0 <= 0 && motorState0 != 0) {
        setMotor(0, 0);
    }

    if (targetValvePosMsec1 > 0 && motorState1 != 0) {
        targetValvePosMsec1 -= delta;
    } else if (targetValvePosMsec1 <= 0 && motorState1 != 0) {
        setMotor(1, 0);
    }

    // If a valve touched a limit switch, stop that valve.
    if (!digitalRead(PIN_VALVE_CLOSE_0) && motorState0 == -1) {
        setMotor(0, 0);
        targetValvePosMsec0 = 0;
    }

    if (!digitalRead(PIN_VALVE_OPEN_0) && motorState0 == 1) {
        setMotor(0, 0);
        targetValvePosMsec0 = 0;
    }

    if (!digitalRead(PIN_VALVE_CLOSE_1) && motorState1 == -1) {
        setMotor(1, 0);
        targetValvePosMsec1 = 0;
    }

    if (!digitalRead(PIN_VALVE_OPEN_1) && motorState1 == 1) {
        setMotor(1, 0);
        targetValvePosMsec1 = 0;
    }
}

// Turn or stop valve motor. direction: 0 = Stop, -1 = Close, 1 = Open
void setMotor(int which, int direction) {
    int outPin0;
    int outPin1;

    // Determine which valve to turn
    switch(which) {
    case 0:
        outPin0 = PIN_VALVE_MOTOR_A0;
        outPin1 = PIN_VALVE_MOTOR_A1;
        motorState0 = direction;
        break;
    case 1:
        outPin0 = PIN_VALVE_MOTOR_B0;
        outPin1 = PIN_VALVE_MOTOR_B1;
        motorState1 = direction;
        break;
    }

    // Move the valves accodingly
    switch(direction) {
    case 0: // STOP
        digitalWrite(outPin0, 0);
        digitalWrite(outPin1, 0);
        break;
    case -1: // CLOSE
        digitalWrite(outPin0, 0);
        digitalWrite(outPin1, 1);
        break;
    case 1: // OPEN
        digitalWrite(outPin0, 1);
        digitalWrite(outPin1, 0);
        break;
    }
}

// Read all I2C ADC values (Including the average value), used for soil sensors.
// Sets: soilHumidity, avgSoilHumidity
void updateSoilHumidity(){
    // Get individual values and apply calibrations
    for(int i; i < C_MOIST_SENS_AMOUNT / 4; i++) {
        soilHumidity[i]    = SOILCALIBRATION(Adc0.readADC_SingleEnded(i));
        soilHumidity[i+4]  = SOILCALIBRATION(Adc1.readADC_SingleEnded(i));
        soilHumidity[i+8]  = SOILCALIBRATION(Adc2.readADC_SingleEnded(i));
        soilHumidity[i+12] = SOILCALIBRATION(Adc3.readADC_SingleEnded(i));
    }

    // Calculate the average soil humidity
    uint16_t currentAvg = soilHumidity[0];
    for (int i = 1; i < C_MOIST_SENS_AMOUNT; i++) {
        currentAvg = (currentAvg + soilHumidity[i]) / 2;
    }
    avgSoilHumidity = currentAvg;
}

// Calculate the current flow rate of the flow sensors
// Sets: flowRate
void updateFlowRate(){
    // disable interrupts
    setInterrupts(false);

    // Convert counted pulses to Liter per Minute
    flowRate[0] = ((1000.0 / reportDelta) * pulseCount0) / C_FLOW_CALIBRATION;
    flowRate[1] = ((1000.0 / reportDelta) * pulseCount1) / C_FLOW_CALIBRATION;

    // Reset pulse counters
    pulseCount0 = 0;
    pulseCount1 = 0;

    // enable interrupts
    setInterrupts(true);
}

// Get temperature and humidity, internal and external
// Sets: enclosureTemperature, airTemperature, airHumidity
// TODO Note: Might return NaN if dht is not found
void updateTemperatureHumidity() {
    enclosureTemperature = Dht1.readTemperature();
    airTemperature = Dht0.readTemperature();
    airHumidity = Dht0.readHumidity();
}

// Attach or detach interrupts for the water flow sensors
void setInterrupts(bool state) {
    if (state) {
        attachInterrupt(digitalPinToInterrupt(PIN_FLOW_METER_0), pulseInterrupt0, FALLING);
        attachInterrupt(digitalPinToInterrupt(PIN_FLOW_METER_1), pulseInterrupt1, FALLING);
    } else {
        detachInterrupt(pulseInterrupt0);
        detachInterrupt(pulseInterrupt1);
    }
}

// Interrupt function for water flow sensor pulses
void pulseInterrupt0() {
    pulseCount0++;
}

// Interrupt function for water flow sensor pulses
void pulseInterrupt1() {
    pulseCount1++;
}

// Print a simple error message in json form
void printError(char* errorMsg) {
    StaticJsonDocument<C_SIZE_MSG> jbuf;
    jbuf["error"] = errorMsg;
    // Send data
    serializeJson(jbuf, Serial);
    // Make sure to print a new line
    Serial.println();
    //Serial.flush();
}

// Print a simple debug message in json form
void printDebug(char* debugMsg) {
    StaticJsonDocument<C_SIZE_MSG> jbuf;
    jbuf["debug"] = debugMsg;
    // Send data
    serializeJson(jbuf, Serial);
    // Make sure to print a new line
    Serial.println();
    //Serial.flush();
}

// Get a json key value, use defaultVal if key is not found
// NOTE: I'm not sure if using StaticJsonDocument for this is right, but it works...
int getJsonKeyValueAsInt(StaticJsonDocument<C_SIZE_INPUT> doc, String key, int defaultVal) {
    JsonVariant outVariant = doc.getMember(key);
    int out = (!outVariant.isNull()) ? outVariant.as<int>() : defaultVal;
    return out;
}
