/*
 * Copyright (C) 2016 The Android Open Source Project
 * Copyright (C) 2018 Shane Francis
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "lights.msm8952"

#include <android-base/logging.h>
#include <fstream>

#include "Light.h"

namespace android {
    namespace hardware {
        namespace light {
            namespace V2_0 {
                namespace implementation {
                    static_assert(LIGHT_FLASH_NONE == static_cast<int>(Flash::NONE),
                                  "Flash::NONE must match legacy value.");
                    static_assert(LIGHT_FLASH_TIMED == static_cast<int>(Flash::TIMED),
                                  "Flash::TIMED must match legacy value.");
                    static_assert(LIGHT_FLASH_HARDWARE == static_cast<int>(Flash::HARDWARE),
                                  "Flash::HARDWARE must match legacy value.");

                    static_assert(BRIGHTNESS_MODE_USER == static_cast<int>(Brightness::USER),
                                  "Brightness::USER must match legacy value.");
                    static_assert(BRIGHTNESS_MODE_SENSOR == static_cast<int>(Brightness::SENSOR),
                                  "Brightness::SENSOR must match legacy value.");
                    static_assert(BRIGHTNESS_MODE_LOW_PERSISTENCE ==
                                  static_cast<int>(Brightness::LOW_PERSISTENCE),
                                  "Brightness::LOW_PERSISTENCE must match legacy value.");

                    Light *Light::sInstance = nullptr;

                    Light::Light() {
                        LOG(INFO) << "%s";
                        openHal();
                        sInstance = this;
                    }

                    void Light::openHal(){
                        LOG(INFO) << __func__ << ": Setup HAL";
                        mDevice = static_cast<lights_t *>(malloc(sizeof(lights_t)));
                        memset(mDevice, 0, sizeof(lights_t));

                        mDevice->g_last_backlight_mode = BRIGHTNESS_MODE_USER;
                        mDevice->g_lock = (pthread_mutex_t) PTHREAD_MUTEX_INITIALIZER;
                        mDevice->g_lcd_lock = (pthread_mutex_t) PTHREAD_MUTEX_INITIALIZER;

                        mDevice->backlight_bits = (readInt(LCD_MAX_FILE) == 4095 ? 12 : 8);
                    }

                    // Methods from ::android::hardware::light::V2_0::ILight follow.
                    Return<Status> Light::setLight(Type type, const LightState &state) {
                        switch (type) {
                            case Type::BACKLIGHT:
                                LOG(DEBUG) << __func__ << " : Type::BACKLIGHT";
                                setLightBacklight(state);
                                break;
                            case Type::BATTERY:
                                LOG(DEBUG) << __func__ << " : Type::BATTERY";
                                setLightBattery(state);
                                break;
                            case Type::NOTIFICATIONS:
                                LOG(DEBUG) << __func__ << " : Type::NOTIFICATIONS";
                                setLightNotifications(state);
                                break;
                            default:
                                LOG(DEBUG) << __func__ << " : Unknown light type";
                                return Status::LIGHT_NOT_SUPPORTED;
                        }
                        return Status::SUCCESS;
                    }

                    ILight *Light::getInstance() {
                        if (!sInstance) {
                            sInstance = new Light();
                        }
                        return sInstance;
                    }

                    int Light::writeInt(const std::string &path, int value) {
                        std::ofstream stream(path);

                        if (!stream) {
                            LOG(ERROR) << "Failed to open " << path << ", error=" << errno
                                       << "(" << strerror(errno) << ")";
                            return -errno;
                        }

                        stream << value << std::endl;

                        return 0;
                    }

                    int Light::readInt(const std::string &path) {
                        std::ifstream stream(path);
                        int value = 0;

                        if (!stream) {
                            LOG(ERROR) << "Failed to open " << path << ", error=" << errno
                                       << "(" << strerror(errno) << ")";
                            return -errno;
                        }

                        stream >> value;

                        return value;
                    }

                    int Light::writeStr(const std::string &path, const std::string &value) {
                        std::ofstream stream(path);

                        if (!stream) {
                            LOG(ERROR) << "Failed to open " << path << ", error=" << errno
                                       << "(" << strerror(errno) << ")";
                            return -errno;
                        }

                        stream << value << std::endl;

                        return 0;
                    }

                    std::string Light::getScaledDutyPcts(int brightness) {
                        std::string buf, pad;

                        for (auto i : BRIGHTNESS_RAMP) {
                            buf += pad;
                            buf += std::to_string(i * brightness / 255);
                            pad = ",";
                        }

                        return buf;
                    }

                    int Light::isLit(const LightState &state) {
                        return state.color & 0x00ffffff;
                    }

                    int Light::rgbToBrightness(const LightState &state) {
                        int color = state.color & 0x00ffffff;
                        return ((77 * ((color >> 16) & 0x00ff))
                                + (150 * ((color >> 8) & 0x00ff)) + (29 * (color & 0x00ff))) >> 8;
                    }

                    int Light::setLightBacklight(const LightState &state) {
                        int err = 0;
                        int brightness = rgbToBrightness(state);
                        unsigned int lpEnabled = state.brightnessMode == Brightness::LOW_PERSISTENCE;

                        if (!mDevice) {
                            return -1;
                        }

                        pthread_mutex_lock(&mDevice->g_lcd_lock);

#ifdef LOW_PERSISTENCE_DISPLAY
                        int currState = static_cast<int>(state.brightnessMode);
                        // If we're not in lp mode and it has been enabled or if we are in lp mode
                        // and it has been disabled send an ioctl to the display with the update
                        if ((mDevice->g_last_backlight_mode != currState && lpEnabled) ||
                                (!lpEnabled && mDevice->g_last_backlight_mode == BRIGHTNESS_MODE_LOW_PERSISTENCE)) {
                            if ((err = writeInt(PERSISTENCE_FILE, lpEnabled)) != 0) {
                                LOG(ERROR) << __func__ << " : Failed to write to " << PERSISTENCE_FILE << ": " << strerror(errno);
                            }
                            if (lpEnabled != 0) {
                                // Try to get the brigntess though property, otherwise it will
                                // set the default brightness, which is defined in BoardConfig.mk.
                                brightness = property_get_int32(LP_MODE_BRIGHTNESS_PROPERTY,
                                        DEFAULT_LOW_PERSISTENCE_MODE_BRIGHTNESS);
                            }
                        }
                        mDevice->g_last_backlight_mode = static_cast<int>(state.brightnessMode);
#endif

                        if (!err) {
                            if (mDevice->backlight_bits > 8) {
                                brightness = brightness << (mDevice->backlight_bits - 8);
                            }

                            err = writeInt(LCD_FILE, brightness);
                        }

                        pthread_mutex_unlock(&mDevice->g_lcd_lock);
                        return err;
                    }


                    int Light::setSpeakerLightLocked(const LightState& state) {
                        int brightness;
                        char pattern[PAGE_SIZE];
                        bool blink;
                        int onS,  offS;
                        unsigned int colorRGB;

                        if (!mDevice) {
                            return -1;
                        }

                        switch (state.flashMode) {
                            case Flash::TIMED:
                                onS = (state.flashOnMs + 999) / 1000;
                                offS = (state.flashOffMs + 999) / 1000;
                                break;
                            case Flash::NONE:
                            default:
                                onS = 0;
                                offS = 0;
                                break;
                        }

                        brightness = (state.color >> 24 ) -1;
                        blink = onS > 0 && offS > 0;

#if 0
                        LOG(INFO) << "set_speaker_light_locked mode " << (int) state.flashMode <<
                                /*" colorRGB=" << colorRGB <<*/ " onM=" << onS << " offM=" << offS << " brightness=" << brightness;
#endif
                        if (blink || (brightness > 0 && brightness < 254)) {
                            if (blink) {
                                sprintf(pattern, "2 %d 2 %d",onS,offS);
                                LOG(INFO) << "Using blink pattern: " << pattern;
                                writeStr(POWER_PATTERN_FILE, pattern);
                                writeInt(POWER_BLINK_FILE,1);
                            } else {
                                LOG(INFO) << "Using brightness: " << brightness;
                                writeInt(POWER_LED_FILE, brightness);
                            }
                        } else {
                            LOG(INFO) << "Turn off power light.";
                            writeInt(POWER_BLINK_FILE,0);
                            writeInt(POWER_LED_FILE,0);
                        }

                        return 0;
                    }

                    void Light::handleSpeakerBatteryLocked() {
                        if (isLit(batteryState)) {
                            setSpeakerLightLocked(batteryState);
                        } else {
                            setSpeakerLightLocked(notificationState);
                        }
                    }

                    int Light::setLightBattery(const LightState& state) {
                        if(!mDevice) {
                            return -1;
                        }

                        pthread_mutex_lock(&mDevice->g_lock);
                        batteryState = state;
                        handleSpeakerBatteryLocked();
                        pthread_mutex_unlock(&mDevice->g_lock);
                        return 0;
                    }

                    int Light::setLightNotifications(const LightState& state) {
                        if(!mDevice) {
                            return -1;
                        }

                        pthread_mutex_lock(&mDevice->g_lock);
                        notificationState = state;
                        handleSpeakerBatteryLocked();
                        pthread_mutex_unlock(&mDevice->g_lock);
                        return 0;
                    }

                    const static std::map<Type, const char *> kLogicalLights = {
                            {Type::BACKLIGHT,     LIGHT_ID_BACKLIGHT},
                            {Type::BATTERY,       LIGHT_ID_BATTERY},
                            {Type::NOTIFICATIONS, LIGHT_ID_NOTIFICATIONS}
                    };

                    Return<void> Light::getSupportedTypes(getSupportedTypes_cb _hidl_cb) {
                        Type *types = new Type[kLogicalLights.size()];

                        int idx = 0;
                        for (auto const &pair : kLogicalLights) {
                            Type type = pair.first;

                            types[idx++] = type;
                        }

                        {
                            hidl_vec<Type> hidl_types{};
                            hidl_types.setToExternal(types, kLogicalLights.size());

                            _hidl_cb(hidl_types);
                        }

                        delete[] types;

                        return Void();
                    }
                } // namespace implementation
            }  // namespace V2_0
        }  // namespace light
    }  // namespace hardware
}  // namespace android
