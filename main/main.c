#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "SSD1306_TEST";

#define I2C_MASTER_SCL_IO 22
#define I2C_MASTER_SDA_IO 21
#define I2C_MASTER_FREQ_HZ 400000 // 400 кГц Fast Mode для быстрой отрисовки
#define SSD1306_I2C_ADDR 0x3C

// Статический буфер кадра: 1 контрольный байт (0x40 - DATA) + 1024 байта
// пикселей Выделяем static в BSS, чтобы не перегружать стек задачи FreeRTOS
static uint8_t oled_buffer[1 + 1024];

// Последовательность инициализации SSD1306 (128x64)
static const uint8_t init_cmds[] = {
    0x00, // Control Byte: поток команд
    0xAE, // Display OFF (перевод в спящий режим перед настройкой)
    0xD5,
    0x80, // Установка делителя частоты осциллятора (стандартный сброс)
    0xA8,
    0x3F, // Multiplex Ratio = 63 (высота 64 строки: 0x3F = 63d)
    0xD3,
    0x00, // Display Offset = 0
    0x40, // Display Start Line = 0 (начинаем с нулевой строки)
    0x8D,
    0x14, // Включение встроенного CHARGE PUMP (7.5V для OLED) - КРИТИЧНО!
    0x20,
    0x00, // Режим адресации: 0x00 = Horizontal Addressing Mode
    0xA1, // Segment Re-map: 0xA1 (зеркалирование по X для правильной
          // ориентации)
    0xC8, // COM Output Scan Direction: 0xC8 (зеркалирование по Y)
    0xDA,
    0x12, // Конфигурация аппаратных выводов COM: альтернативная разводка
    0x81,
    0xCF, // Установка контрастности (яркости) = 0xCF
    0xD9,
    0xF1, // Период предзаряда матрицы (Phase 1 = 1 DCLK, Phase 2 = 15 DCLK)
    0xDB,
    0x40, // Vcomh уровень детекции
    0xA4, // Вывод следует содержимому RAM (не зажигать всё принудительно)
    0xA6, // Normal Display (1 = светится, 0 = погашен)
    0xAF  // Display ON! Включение питания панели
};

void app_main(void) {
  ESP_LOGI(TAG, "Старт инициализации шины и SSD1306...");

  // 1. Конфигурация и запуск шины
  i2c_master_bus_config_t bus_config = {
      .i2c_port = I2C_NUM_0,
      .sda_io_num = I2C_MASTER_SDA_IO,
      .scl_io_num = I2C_MASTER_SCL_IO,
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = 7,
      .flags.enable_internal_pullup = true,
  };
  i2c_master_bus_handle_t bus_handle;
  ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &bus_handle));

  // 2. Добавление устройства SSD1306 на шину
  i2c_device_config_t dev_config = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address = SSD1306_I2C_ADDR,
      .scl_speed_hz = I2C_MASTER_FREQ_HZ,
  };
  i2c_master_dev_handle_t dev_handle;
  ESP_ERROR_CHECK(
      i2c_master_bus_add_device(bus_handle, &dev_config, &dev_handle));

  // 3. Отправка цепочки инициализации
  ESP_LOGI(TAG, "Отправка команд инициализации SSD1306...");
  ESP_ERROR_CHECK(
      i2c_master_transmit(dev_handle, init_cmds, sizeof(init_cmds), 100));

  // 4. Формирование тестового рисунка в буфере
  // Нулевой байт - префикс данных I2C
  oled_buffer[0] = 0x40;

  // Заполняем страницы тестовым узором: чередование полос 0xAA (10101010b) и
  // 0x55 (01010101b)
  for (int i = 1; i <= 1024; i++) {
    oled_buffer[i] = ((i / 64) % 2 == 0) ? 0xAA : 0x55;
  }

  // 5. Передача буфера кадра (1025 байт одной транзакцией)
  ESP_LOGI(TAG, "Заливка тестового кадра в дисплей...");
  ESP_ERROR_CHECK(
      i2c_master_transmit(dev_handle, oled_buffer, sizeof(oled_buffer), 1000));

  ESP_LOGI(TAG,
           "Экран должен светиться! Запуск цикла инверсии каждые 1.5 сек...");

  // 6. Демонстрация живой работы: инверсия экрана (0xA7 - инверсия, 0xA6 -
  // норма)
  bool inverted = false;
  while (1) {
    vTaskDelay(pdMS_TO_TICKS(1500));
    inverted = !inverted;
    uint8_t cmd[2] = {0x00, inverted ? 0xA7 : 0xA6};
    i2c_master_transmit(dev_handle, cmd, sizeof(cmd), 100);
    ESP_LOGI(TAG, "Инверсия дисплея: %s", inverted ? "ВКЛ" : "ВЫКЛ");
  }
}
