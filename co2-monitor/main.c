/*
 *   Please read the disclaimer
 *
 *
 *   DISCLAIMER
 *
 *   The learning_path_libs functions provided in the learning_path_libs folder:
 *
 *	   1. are NOT supported Azure Sphere APIs.
 *	   2. are prefixed with lp_, typedefs are prefixed with LP_
 *	   3. are built from the Azure Sphere SDK Samples at https://github.com/Azure/azure-sphere-samples
 *	   4. are not intended as a substitute for understanding the Azure Sphere SDK Samples.
 *	   5. aim to follow best practices as demonstrated by the Azure Sphere SDK Samples.
 *	   6. are provided as is and as a convenience to aid the Azure Sphere Developer Learning experience.
 *
 */

#include "hw/azure_sphere_learning_path.h"

#include "learning_path_libs/azure_iot.h"
#include "learning_path_libs/exit_codes.h"
#include "learning_path_libs/globals.h"
#include "learning_path_libs/peripheral_gpio.h"
#include "learning_path_libs/terminate.h"
#include "learning_path_libs/timer.h"

#include "applibs_versions.h"
#include <applibs/gpio.h>
#include <applibs/log.h>
#include <applibs/powermanagement.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>

#include "./embedded-scd/scd30/scd30.h"


#define JSON_MESSAGE_BYTES 256 // Number of bytes to allocate for the JSON telemetry message for IoT Central

// Forward signatures
static void MeasureSensorHandler(EventLoopTimer* eventLoopTimer);
static void NetworkConnectionStatusHandler(EventLoopTimer* eventLoopTimer);
static void DeviceTwinGenericHandler(LP_DEVICE_TWIN_BINDING* deviceTwinBinding);

static char msgBuffer[JSON_MESSAGE_BYTES] = { 0 };

enum HVAC { HEATING, COOLING, OFF };
static char* hvacState[] = { "Heating", "Cooling", "Off" };

static enum HVAC current_hvac_state = OFF;
static float co2_ppm, temperature, relative_humidity = 0.0;

// GPIO Output PeripheralGpios
#ifdef OEM_SEEED_STUDIO
static LP_PERIPHERAL_GPIO hvacHeatingLed = { .pin = LED_RED, .direction = LP_OUTPUT, .initialState = GPIO_Value_Low, .initialise = lp_openPeripheralGpio, .name = "hvacHeatingLed" };
static LP_PERIPHERAL_GPIO hvacCoolingLed = { .pin = LED_BLUE, .direction = LP_OUTPUT, .initialState = GPIO_Value_Low, .initialise = lp_openPeripheralGpio, .name = "hvacCoolingLed" };
static LP_PERIPHERAL_GPIO co2AlertLed = { .pin = LED_GREEN, .direction = LP_OUTPUT, .initialState = GPIO_Value_Low, .initialise = lp_openPeripheralGpio, .name = "co2AlertLed" };
#endif // OEM_AVNET

#ifdef OEM_AVNET
static LP_PERIPHERAL_GPIO hvacHeatingLed = { .pin = LED_RED, .direction = LP_OUTPUT, .initialState = GPIO_Value_Low, .invertPin = true, .initialise = lp_openPeripheralGpio, .name = "hvacHeatingLed" };
static LP_PERIPHERAL_GPIO hvacCoolingLed = { .pin = LED_BLUE, .direction = LP_OUTPUT, .initialState = GPIO_Value_Low, .invertPin = true, .initialise = lp_openPeripheralGpio, .name = "hvacCoolingLed" };
static LP_PERIPHERAL_GPIO co2AlertLed = { .pin = LED_GREEN, .direction = LP_OUTPUT, .initialState = GPIO_Value_Low, .invertPin = true, .initialise = lp_openPeripheralGpio, .name = "co2AlertLed" };
#endif // OEM_AVNET

static LP_PERIPHERAL_GPIO azureConnectedLed = { .pin = NETWORK_CONNECTED_LED, .direction = LP_OUTPUT, .initialState = GPIO_Value_Low, .invertPin = true, .initialise = lp_openPeripheralGpio, .name = "azureConnectedLed" };

// Timers
static LP_TIMER azureConnectedStatusTimer = { .period = {10, 0}, .name = "azureConnectedStatusTimer", .handler = NetworkConnectionStatusHandler };
static LP_TIMER measureSensorTimer = { .period = {60, 0}, .name = "measureSensorTimer", .handler = MeasureSensorHandler };

// Azure IoT Device Twins
static LP_DEVICE_TWIN_BINDING desiredTemperature = { .twinProperty = "DesiredTemperature", .twinType = LP_TYPE_FLOAT, .handler = DeviceTwinGenericHandler };
static LP_DEVICE_TWIN_BINDING desiredCO2AlertLevel = { .twinProperty = "DesiredCO2AlertLevel", .twinType = LP_TYPE_FLOAT, .handler = DeviceTwinGenericHandler };
static LP_DEVICE_TWIN_BINDING actualTemperature = { .twinProperty = "ActualTemperature", .twinType = LP_TYPE_FLOAT };
static LP_DEVICE_TWIN_BINDING actualCO2Level = { .twinProperty = "ActualCO2Level", .twinType = LP_TYPE_FLOAT };
static LP_DEVICE_TWIN_BINDING actualHvacState = { .twinProperty = "ActualHvacState", .twinType = LP_TYPE_STRING };

// Initialize Sets
LP_PERIPHERAL_GPIO* PeripheralGpioSet[] = { &hvacHeatingLed, &hvacCoolingLed, &co2AlertLed, &azureConnectedLed };
LP_DEVICE_TWIN_BINDING* deviceTwinBindingSet[] = { &desiredTemperature, &actualTemperature, &desiredCO2AlertLevel, &actualCO2Level, &actualHvacState };
LP_TIMER* timerSet[] = { &azureConnectedStatusTimer, &measureSensorTimer };

/// <summary>
/// Check status of connection to Azure IoT
/// </summary>
static void NetworkConnectionStatusHandler(EventLoopTimer* eventLoopTimer)
{
	if (ConsumeEventLoopTimerEvent(eventLoopTimer) != 0)
	{
		lp_terminate(ExitCode_ConsumeEventLoopTimeEvent);
		return;
	}

	if (lp_connectToAzureIot()) { lp_gpioOn(&azureConnectedLed); }
	else { lp_gpioOff(&azureConnectedLed); }
}

/// <summary>
/// Set the HVAC status led. 
/// Red if heater needs to be turned on to get to desired temperature. 
/// Blue to turn on cooler. 
/// Green equals just right, no action required.
/// </summary>
static void SetTemperatureStatusColour(void)
{
	if (!desiredTemperature.twinStateUpdated) { return; }

	static enum HVAC previous_hvac_state = OFF;
	int actual = (int)temperature;
	int desired = (int)*(float*)desiredTemperature.twinState;

	current_hvac_state = actual == desired ? OFF : actual > desired ? COOLING : HEATING;

	if (previous_hvac_state != current_hvac_state)
	{
		previous_hvac_state = current_hvac_state;
		lp_deviceTwinReportState(&actualHvacState, hvacState[current_hvac_state]);
	}

	lp_gpioOff(&hvacHeatingLed);
	lp_gpioOff(&hvacCoolingLed);

	switch (current_hvac_state)
	{
	case HEATING:
		lp_gpioOn(&hvacHeatingLed);
		break;
	case COOLING:
		lp_gpioOn(&hvacCoolingLed);
		break;
	default:
		break;
	}
}

static void SetCO2AlertStatus(void)
{
	if (!desiredCO2AlertLevel.twinStateUpdated) { return; }

	if (co2_ppm > *(float*)desiredCO2AlertLevel.twinState) 
	{
		lp_gpioOn(&co2AlertLed);
	}
	else
	{
		lp_gpioOff(&co2AlertLed);
	}
}

/// <summary>
/// Read sensor and send to Azure IoT
/// </summary>
static void MeasureSensorHandler(EventLoopTimer* eventLoopTimer)
{
	static int msgId = 0;
	static const char* MsgTemplate = "{ \"CO2\": \"%3.2f\", \"Temperature\": \"%3.2f\", \"Humidity\": \"%3.1f\", \"MsgId\":%d }";

	if (ConsumeEventLoopTimerEvent(eventLoopTimer) != 0)
	{
		lp_terminate(ExitCode_ConsumeEventLoopTimeEvent);
		return;
	}

	if (scd30_read_measurement(&co2_ppm, &temperature, &relative_humidity) == STATUS_OK)
	{
		if (!isnan(co2_ppm))
		{
			SetTemperatureStatusColour();
			SetCO2AlertStatus();

			if (snprintf(msgBuffer, JSON_MESSAGE_BYTES, MsgTemplate, co2_ppm, temperature, relative_humidity, ++msgId) > 0)
			{
				Log_Debug("%s\n", msgBuffer);
				lp_sendMsg(msgBuffer);
			}
			lp_deviceTwinReportState(&actualTemperature, &temperature);
			lp_deviceTwinReportState(&actualCO2Level, &co2_ppm);
		}
	}
}

/// <summary>
/// Generic Device Twin Handler. It just sets reported state for the twin
/// </summary>
static void DeviceTwinGenericHandler(LP_DEVICE_TWIN_BINDING* deviceTwinBinding)
{
	lp_deviceTwinReportState(deviceTwinBinding, deviceTwinBinding->twinState);

	SetTemperatureStatusColour();
	SetCO2AlertStatus();
}

static bool InitializeSdc30(void)
{
	uint16_t interval_in_seconds = 2;
	int retry = 0;
	uint8_t asc_enabled, enable_asc;

	sensirion_i2c_init();

	while (scd30_probe() != STATUS_OK && ++retry < 5)
	{
		printf("SCD30 sensor probing failed\n");
		sensirion_sleep_usec(1000000u);
	}

	if (retry >= 5) { return false; }

	/*
	When scd30 automatic self calibration activated for the first time a period of minimum 7 days is needed so
	that the algorithm can find its initial parameter set for ASC. The sensor has to be exposed to fresh air for at least 1 hour every day.
	Refer to the datasheet for further conditions and scd30.h for more info.
	*/

	if (scd30_get_automatic_self_calibration(&asc_enabled) == 0)
	{
		if (asc_enabled == 0)
		{
			enable_asc = 1;
			if (scd30_enable_automatic_self_calibration(enable_asc) == 0)
			{
				Log_Debug("scd30 automatic self calibration enabled. Takes 7 days, at least 1 hour/day outside, powered continuously");
			}
		}
	}

	scd30_set_measurement_interval(interval_in_seconds);
	sensirion_sleep_usec(20000u);
	scd30_start_periodic_measurement(0);
	sensirion_sleep_usec(interval_in_seconds * 1000000u);

	return true;
}

/// <summary>
///  Initialize PeripheralGpios, device twins, direct methods, timers.
/// </summary>
/// <returns>0 on success, or -1 on failure</returns>
static void InitPeripheralGpiosAndHandlers(void)
{
	InitializeSdc30();

	lp_openPeripheralGpioSet(PeripheralGpioSet, NELEMS(PeripheralGpioSet));
	lp_openDeviceTwinSet(deviceTwinBindingSet, NELEMS(deviceTwinBindingSet));

	lp_startTimerSet(timerSet, NELEMS(timerSet));

	lp_startCloudToDevice();
}

/// <summary>
///     Close PeripheralGpios and handlers.
/// </summary>
static void ClosePeripheralGpiosAndHandlers(void)
{
	Log_Debug("Closing file descriptors\n");

	lp_stopTimerSet();

	lp_stopCloudToDevice();

	lp_closePeripheralGpioSet();
	lp_closeDeviceTwinSet();

	scd30_stop_periodic_measurement();

	lp_stopTimerEventLoop();
}

int main(int argc, char* argv[])
{
	lp_registerTerminationHandler();
	lp_processCmdArgs(argc, argv);

	if (strlen(scopeId) == 0)
	{
		Log_Debug("ScopeId needs to be set in the app_manifest CmdArgs\n");
		return ExitCode_Missing_ID_Scope;
	}

	InitPeripheralGpiosAndHandlers();

	// Main loop
	while (!lp_isTerminationRequired())
	{
		int result = EventLoop_Run(lp_getTimerEventLoop(), -1, true);
		// Continue if interrupted by signal, e.g. due to breakpoint being set.
		if (result == -1 && errno != EINTR)
		{
			lp_terminate(ExitCode_Main_EventLoopFail);
		}
	}

	ClosePeripheralGpiosAndHandlers();

	Log_Debug("Application exiting.\n");
	return lp_getTerminationExitCode();
}