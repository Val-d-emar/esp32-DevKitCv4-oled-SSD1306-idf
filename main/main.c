#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>

static const char *TAG = "I2C_SCANNER";

#define I2C_MASTER_SCL_IO 22      // Пин SCL
#define I2C_MASTER_SDA_IO 21      // Пин SDA
#define I2C_MASTER_FREQ_HZ 100000 // Стандартная частота 100 кГц (Standard Mode)

void app_main(void) {
  ESP_LOGI(TAG, "Инициализация шины I2C...");

  // Конфигурация шины I2C Master
  i2c_master_bus_config_t bus_config = {
      .i2c_port = I2C_NUM_0,
      .sda_io_num = I2C_MASTER_SDA_IO,
      .scl_io_num = I2C_MASTER_SCL_IO,
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = 7,
      .flags.enable_internal_pullup =
          true, // Включаем внутреннюю подтяжку
  };

  i2c_master_bus_handle_t bus_handle;
  ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &bus_handle));

  ESP_LOGI(TAG, "Шина I2C готова. Запуск сканирования адресного пространства "
                "(0x01 - 0x7F)...");

  int devices_found = 0;

  // Сканируем стандартный 7-битный диапазон адресов
  for (uint16_t addr = 0x01; addr < 0x80; addr++) {
    // i2c_master_probe отправляет START + (addr << 1 | WRITE) и ждет ACK
    esp_err_t ret = i2c_master_probe(bus_handle, addr, 50); // Таймаут 50 мс
    if (ret == ESP_OK) {
      ESP_LOGI(TAG, "-> Найдено I2C устройство по адресу: 0x%02X (%d)", addr,
               addr);
      devices_found++;
    }
  }

  if (devices_found == 0) {
    ESP_LOGW(TAG, "Устройства не обнаружены! Проверь питание, контакты SDA/SCL "
                  "и подтяжку.");
  } else {
    ESP_LOGI(TAG, "Сканирование завершено. Всего найдено устройств: %d",
             devices_found);
  }

  // Удаляем шину после теста
  ESP_ERROR_CHECK(i2c_del_master_bus(bus_handle));
}
