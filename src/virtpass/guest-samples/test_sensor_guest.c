/*
 * test_sensor_guest.c - Test program for Android NDK API proxy
 *
 * This is a RISC-V Guest program that tests the sensor API proxy
 * pipeline. It calls vp_ndk_stub functions which will be proxied
 * to the host side via custom syscalls.
 *
 * Build:
 *   riscv64-linux-android35-clang -static -o test_sensor_guest \
 *       test_sensor_guest.c -L../vp_ndk_stub -landroid_stubs
 *
 * Run:
 *   rvvm-user test_sensor_guest
 */

#include <stdio.h>
#include <string.h>
#include "virtpass/vp_android.h"

int main(int argc, char** argv)
{
    printf("=== Android NDK API Proxy Test ===\n");
    printf("Guest: RISC-V 64-bit\n");
    printf("Args: %d\n", argc);

    for (int i = 0; i < argc; i++) {
        printf("  argv[%d] = %s\n", i, argv[i]);
    }

    /* Test 1: Sensor Manager */
    printf("\n--- Test 1: Sensor Manager ---\n");
    ASensorManager* manager = ASensorManager_getInstance();
    if (manager) {
        printf("Sensor manager: OK\n");
    } else {
        printf("Sensor manager: FAILED\n");
        return 1;
    }

    /* Test 2: Get sensor list */
    printf("\n--- Test 2: Sensor List ---\n");
    ASensor const* sensor_list[16];
    int sensor_count = ASensorManager_getSensorList(manager, sensor_list);
    printf("Sensor count: %d\n", sensor_count);

    for (int i = 0; i < sensor_count; i++) {
        printf("  Sensor %d: %s (vendor: %s, type: %d, handle: %d)\n",
               i,
               ASensor_getName(sensor_list[i]),
               ASensor_getVendor(sensor_list[i]),
               ASensor_getType(sensor_list[i]),
               ASensor_getHandle(sensor_list[i]));
    }

    /* Test 3: Get default accelerometer */
    printf("\n--- Test 3: Default Accelerometer ---\n");
    ASensor const* accel = ASensorManager_getDefaultSensor(manager, ASENSOR_TYPE_ACCELEROMETER);
    if (accel) {
        printf("Accelerometer: %s\n", ASensor_getName(accel));
        printf("  Vendor: %s\n", ASensor_getVendor(accel));
        printf("  Resolution: %f\n", ASensor_getResolution(accel));
        printf("  Min Delay: %d us\n", ASensor_getMinDelay(accel));
    } else {
        printf("Accelerometer: NOT FOUND\n");
    }

    /* Test 4: Create event queue */
    printf("\n--- Test 4: Event Queue ---\n");
    ASensorEventQueue* queue = ASensorManager_createEventQueue(manager, NULL, 0, NULL, NULL);
    if (queue) {
        printf("Event queue: OK\n");
    } else {
        printf("Event queue: FAILED\n");
    }

    /* Test 5: Enable sensor */
    printf("\n--- Test 5: Enable Sensor ---\n");
    if (accel) {
        int result = ASensorEventQueue_enableSensor(queue, accel);
        printf("Enable accelerometer: %s\n", result == 0 ? "OK" : "FAILED");
    }

    /* Test 6: Check for events */
    printf("\n--- Test 6: Check Events ---\n");
    int has_events = ASensorEventQueue_hasEvents(queue);
    printf("Has events: %s\n", has_events > 0 ? "YES" : "NO");

    /* Test 7: Get events (should return 0 events) */
    printf("\n--- Test 7: Get Events ---\n");
    ASensorEvent events[8];
    ssize_t event_count = ASensorEventQueue_getEvents(queue, events, 8);
    printf("Event count: %zd\n", event_count);

    /* Test 8: Disable sensor */
    printf("\n--- Test 8: Disable Sensor ---\n");
    if (accel) {
        int result = ASensorEventQueue_disableSensor(queue, accel);
        printf("Disable accelerometer: %s\n", result == 0 ? "OK" : "FAILED");
    }

    /* Test 9: Window API */
    printf("\n--- Test 9: Window API ---\n");
    ANativeWindow* window = ANativeWindow_acquire(NULL);
    if (window) {
        printf("Window: OK (width=%d, height=%d)\n",
               ANativeWindow_getWidth(window),
               ANativeWindow_getHeight(window));
        ANativeWindow_release(window);
    } else {
        printf("Window: FAILED\n");
    }

    /* Test 10: Looper API */
    printf("\n--- Test 10: Looper API ---\n");
    ALooper* looper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
    if (looper) {
        printf("Looper: OK\n");
    } else {
        printf("Looper: FAILED\n");
    }

    printf("\n=== All Tests Complete ===\n");
    return 0;
}
