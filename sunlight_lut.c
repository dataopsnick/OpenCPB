#include "sunlight_lut.h"
#include <modbus.h> // Include the Modbus library for RS-485 communication
#include <curl/curl.h> // For HTTP API calls to the utility's limit order book
#include <stdio.h>
#include <stdlib.h>
#include <FreeRTOS.h>
#include <task.h>

// LUT arrays to hold sunrise and sunset times
double sunriseTable[DAYS_IN_YEAR];
double sunsetTable[DAYS_IN_YEAR];

// Modbus context
modbus_t *ctx;

// Generate the LUT for sunrise and sunset times
void generateSunlightLUT() {
    for (int day = 0; day < DAYS_IN_YEAR; day++) {
        // Calculate solar declination (in degrees)
        double declination = -23.44 * cos((2 * M_PI / 365.0) * (day + 10));

        // Calculate solar noon (in hours)
        double solarNoon = 12.0 - (LONGITUDE / 15.0) - TIMEZONE_OFFSET;

        // Calculate hour angle (in degrees)
        double hourAngle = acos(-tan(LATITUDE * M_PI / 180.0) * tan(declination * M_PI / 180.0)) * 180.0 / M_PI;

        // Calculate sunrise and sunset times (in hours)
        sunriseTable[day] = solarNoon - (hourAngle / 15.0);
        sunsetTable[day] = solarNoon + (hourAngle / 15.0);
    }
}

// Retrieve sunrise and sunset times for the current day
void getSunlightHours(double *sunrise, double *sunset) {
    // Get the current date
    time_t currentTime = time(NULL);
    struct tm *localTime = localtime(&currentTime);

    // Calculate the day of the year (0-364 for indexing)
    int dayOfYear = localTime->tm_yday;

    // Retrieve values from LUT
    *sunrise = sunriseTable[dayOfYear];
    *sunset = sunsetTable[dayOfYear];
}

// RTOS task to handle SOC spoofing and anti-flutter
void SpoofSOC(void *pvParameters) {
    time_t currentTime;
    time_t lastSpoofTime = 0;
    uint16_t actualSOC;

    for (;;) {
        currentTime = time(NULL);

        // Read actual SOC from BMS via Modbus
        if (modbus_read_input_registers(ctx, 0x208, 1, &actualSOC) == -1) {
            fprintf(stderr, "Failed to read SOC register: %s\n", modbus_strerror(errno));
            continue;
        }

        // Enforce 20% SOC safety latch
        if (actualSOC < MIN_SOC) {
            printf("SOC below 20%. Disabling DR events.\n");
            continue;
        }

        // Enforce anti-flutter timer
        if (difftime(currentTime, lastSpoofTime) >= SPOOF_INTERVAL_SECONDS) {
            // Perform DR event (e.g., discharge battery)
            lastSpoofTime = currentTime;
        }

        vTaskDelay(pdMS_TO_TICKS(1000)); // Run every second
    }
}

// RTOS task to handle Fast DR Dispatch
void FastDRDispatch(void *pvParameters) {
    time_t currentTime;
    uint16_t actualSOC;
    bool isDemandResponseActive = false; // Replace with actual DR event state

    for (;;) {
        currentTime = time(NULL);

        // Read actual SOC from BMS via Modbus
        if (modbus_read_input_registers(ctx, 0x208, 1, &actualSOC) == -1) {
            fprintf(stderr, "Failed to read SOC register: %s\n", modbus_strerror(errno));
            continue;
        }

        // Fast DR Dispatch Logic
        if (isDemandResponseActive) {
            double dischargeRate = (actualSOC / 100.0) * MAX_DISCHARGE_RATE;
            modbus_write_register(ctx, 0x210, (uint16_t)dischargeRate); // Adjust discharge rate
        }

        vTaskDelay(pdMS_TO_TICKS(1000)); // Run every second
    }
}

// RTOS task to handle Capacity Bidding
void CapacityBidding(void *pvParameters) {
    time_t currentTime;
    uint16_t actualSOC;
    bool isDemandResponseActive = false; // Replace with actual DR event state

    for (;;) {
        currentTime = time(NULL);

        // Read actual SOC from BMS via Modbus
        if (modbus_read_input_registers(ctx, 0x208, 1, &actualSOC) == -1) {
            fprintf(stderr, "Failed to read SOC register: %s\n", modbus_strerror(errno));
            continue;
        }

        // Capacity Bidding Logic
        if (isDemandResponseActive) {
            double bidPrice = (100.0 - actualSOC) * BID_PRICE_FACTOR;
            submitBid(bidPrice);
        }

        vTaskDelay(pdMS_TO_TICKS(1000)); // Run every second
    }
}

// Submit the bid to the utility's limit order book
void submitBid(double bidPrice) {
    CURL *curl;
    CURLcode res;

    curl_global_init(CURL_GLOBAL_DEFAULT);
    curl = curl_easy_init();

    if (curl) {
        char url[256];
        snprintf(url, sizeof(url), "https://opencpb.cybergolem.io/api/bid?price=%.2f", bidPrice);

        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_POST, 1L);

        // Perform the request
        res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            fprintf(stderr, "Failed to submit bid: %s\n", curl_easy_strerror(res));
        }

        curl_easy_cleanup(curl);
    }

    curl_global_cleanup();
}

// RTOS Initialization
void initSystem() {
    // Generate sunlight LUT
    generateSunlightLUT();

    // Initialize Modbus connection
    ctx = modbus_new_rtu("/dev/ttyUSB0", 9600, 'N', 8, 1);
    if (ctx == NULL) {
        fprintf(stderr, "Unable to create the libmodbus context\n");
        return;
    }
    if (modbus_connect(ctx) == -1) {
        fprintf(stderr, "Connection failed: %s\n", modbus_strerror(errno));
        modbus_free(ctx);
        return;
    }

    // Create RTOS tasks
    xTaskCreate(SpoofSOC, "SpoofSOC", configMINIMAL_STACK_SIZE, NULL, 1, NULL);
    xTaskCreate(FastDRDispatch, "FastDRDispatch", configMINIMAL_STACK_SIZE, NULL, 1, NULL);
    xTaskCreate(CapacityBidding, "CapacityBidding", configMINIMAL_STACK_SIZE, NULL, 1, NULL);

    // Start RTOS scheduler
    vTaskStartScheduler();
}

int main() {
    initSystem();
    while (1) {
        // Main loop (not reached in RTOS)
    }
}
