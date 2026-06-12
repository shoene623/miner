#include <Arduino.h>
#include "display.h"

#ifdef NO_DISPLAY
DisplayDriver *currentDisplayDriver = &noDisplayDriver;
#endif

#ifdef M5STACK_DISPLAY
DisplayDriver *currentDisplayDriver = &m5stackDisplayDriver;
#endif

#ifdef WT32_DISPLAY
DisplayDriver *currentDisplayDriver = &wt32DisplayDriver;
#endif

#ifdef LED_DISPLAY
DisplayDriver *currentDisplayDriver = &ledDisplayDriver;
#endif

#ifdef OLED_042_DISPLAY
DisplayDriver *currentDisplayDriver = &oled042DisplayDriver;
#endif

#ifdef T_DISPLAY
DisplayDriver *currentDisplayDriver = &tDisplayDriver;
#endif

#ifdef AMOLED_DISPLAY
DisplayDriver *currentDisplayDriver = &amoledDisplayDriver;
#endif

#ifdef DONGLE_DISPLAY
DisplayDriver *currentDisplayDriver = &dongleDisplayDriver;
#endif

#ifdef ESP32_2432S028R
DisplayDriver *currentDisplayDriver = &esp32_2432S028RDriver;
#endif

#ifdef ESP32_2432S028_2USB
DisplayDriver *currentDisplayDriver = &esp32_2432S028RDriver;
#endif

#ifdef T_QT_DISPLAY
DisplayDriver *currentDisplayDriver = &t_qtDisplayDriver;
#endif

#ifdef V1_DISPLAY
DisplayDriver *currentDisplayDriver = &tDisplayV1Driver;
#endif

#ifdef M5STICKC_DISPLAY
DisplayDriver *currentDisplayDriver = &m5stickCDriver;
#endif

#ifdef M5STICKCPLUS_DISPLAY
DisplayDriver *currentDisplayDriver = &m5stickCPlusDriver;
#endif

#ifdef T_HMI_DISPLAY
DisplayDriver *currentDisplayDriver = &t_hmiDisplayDriver;
#endif

#ifdef ST7735S_DISPLAY
DisplayDriver *currentDisplayDriver = &sp_kcDisplayDriver;
#endif


// Initialize the display
void initDisplay()
{
  if (!currentDisplayDriver) {
    Serial.println("[ERROR] initDisplay: currentDisplayDriver is NULL!");
    return;
  }
  if (!currentDisplayDriver->initDisplay) {
    Serial.println("[ERROR] initDisplay: initDisplay function pointer is NULL!");
    return;
  }
  currentDisplayDriver->initDisplay();
}

// Alternate screen state
void alternateScreenState()
{
  if (!currentDisplayDriver) {
    Serial.println("[ERROR] alternateScreenState: currentDisplayDriver is NULL!");
    return;
  }
  if (!currentDisplayDriver->alternateScreenState) {
    Serial.println("[ERROR] alternateScreenState: alternateScreenState function pointer is NULL!");
    return;
  }
  currentDisplayDriver->alternateScreenState();
}

// Alternate screen rotation
void alternateScreenRotation()
{
  if (!currentDisplayDriver) {
    Serial.println("[ERROR] alternateScreenRotation: currentDisplayDriver is NULL!");
    return;
  }
  if (!currentDisplayDriver->alternateScreenRotation) {
    Serial.println("[ERROR] alternateScreenRotation: alternateScreenRotation function pointer is NULL!");
    return;
  }
  currentDisplayDriver->alternateScreenRotation();
}

// Draw the loading screen
void drawLoadingScreen()
{
  if (!currentDisplayDriver) {
    Serial.println("[ERROR] drawLoadingScreen: currentDisplayDriver is NULL!");
    return;
  }
  if (!currentDisplayDriver->loadingScreen) {
    Serial.println("[ERROR] drawLoadingScreen: loadingScreen function pointer is NULL!");
    return;
  }
  currentDisplayDriver->loadingScreen();
}

// Draw the setup screen
void drawSetupScreen()
{
  if (!currentDisplayDriver) {
    Serial.println("[ERROR] drawSetupScreen: currentDisplayDriver is NULL!");
    return;
  }
  if (!currentDisplayDriver->setupScreen) {
    Serial.println("[ERROR] drawSetupScreen: setupScreen function pointer is NULL!");
    return;
  }
  currentDisplayDriver->setupScreen();
}

// Reset the current cyclic screen to the first one
void resetToFirstScreen()
{
  if (!currentDisplayDriver) {
    Serial.println("[ERROR] resetToFirstScreen: currentDisplayDriver is NULL!");
    return;
  }
  Serial.println("[DEBUG] Resetting to first cyclic screen (index 0)");
  currentDisplayDriver->current_cyclic_screen = 0;
}

// Switches to the next cyclic screen without drawing it
void switchToNextScreen()
{
  if (!currentDisplayDriver) {
    Serial.println("[ERROR] switchToNextScreen: currentDisplayDriver is NULL!");
    return;
  }
  if (currentDisplayDriver->num_cyclic_screens <= 0) {
    Serial.println("[ERROR] switchToNextScreen: num_cyclic_screens <= 0!");
    return;
  }
  int next = (currentDisplayDriver->current_cyclic_screen + 1) % currentDisplayDriver->num_cyclic_screens;
  Serial.printf("[DEBUG] switchToNextScreen: %d -> %d\n", currentDisplayDriver->current_cyclic_screen, next);
  currentDisplayDriver->current_cyclic_screen = next;
}

// Draw the current cyclic screen
void drawCurrentScreen(unsigned long mElapsed)
{
  if (!currentDisplayDriver) {
    Serial.println("[ERROR] drawCurrentScreen: currentDisplayDriver is NULL!");
    return;
  }
  if (!currentDisplayDriver->cyclic_screens) {
    Serial.println("[ERROR] drawCurrentScreen: cyclic_screens array is NULL!");
    return;
  }
  if (currentDisplayDriver->num_cyclic_screens <= 0) {
    Serial.println("[ERROR] drawCurrentScreen: num_cyclic_screens is <= 0!");
    return;
  }
  if (currentDisplayDriver->current_cyclic_screen < 0 || 
      currentDisplayDriver->current_cyclic_screen >= currentDisplayDriver->num_cyclic_screens) {
    Serial.printf("[ERROR] drawCurrentScreen: current_cyclic_screen index %d out of bounds (0 to %d). Resetting to 0.\n",
                  currentDisplayDriver->current_cyclic_screen, currentDisplayDriver->num_cyclic_screens - 1);
    currentDisplayDriver->current_cyclic_screen = 0;
  }

  Serial.printf("[DEBUG] Drawing cyclic screen index: %d / %d\n", 
                currentDisplayDriver->current_cyclic_screen, currentDisplayDriver->num_cyclic_screens);

  CyclicScreenFunction drawFunc = currentDisplayDriver->cyclic_screens[currentDisplayDriver->current_cyclic_screen];
  if (!drawFunc) {
    Serial.printf("[ERROR] drawCurrentScreen: draw function for screen index %d is NULL!\n", 
                  currentDisplayDriver->current_cyclic_screen);
    return;
  }

  drawFunc(mElapsed);

  Serial.printf("[DEBUG] Done drawing cyclic screen index: %d\n", currentDisplayDriver->current_cyclic_screen);
}

// Animate the current cyclic screen
void animateCurrentScreen(unsigned long frame)
{
  if (!currentDisplayDriver) {
    return;
  }
  if (!currentDisplayDriver->animateCurrentScreen) {
    return;
  }
  currentDisplayDriver->animateCurrentScreen(frame);
}

// Do LED stuff
void doLedStuff(unsigned long frame)
{
  if (!currentDisplayDriver) {
    return;
  }
  if (!currentDisplayDriver->doLedStuff) {
    return;
  }
  currentDisplayDriver->doLedStuff(frame);
}

