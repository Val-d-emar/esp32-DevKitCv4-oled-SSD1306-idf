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

// ============================================================================
// Графические примитивы для работы с буфером
// ============================================================================

// Очистка буфера экрана (заливка черным)
static void oled_clear(void) { memset(&oled_buffer[1], 0x00, 1024); }

// Заливка всего экрана (белый экран)
static void oled_fill(void) { memset(&oled_buffer[1], 0xFF, 1024); }

// Установка состояния одного пикселя (x: 0..127, y: 0..63)
static void oled_draw_pixel(int x, int y, bool color) {
  // Защита от выхода за границы массива (Out of Bounds)
  if (x < 0 || x >= 128 || y < 0 || y >= 64) {
    return;
  }

  // Вычисляем индекс байта и маску бита
  int page = y / 8;
  int bit = y % 8;
  int index = 1 + (page * 128) + x;

  if (color) {
    oled_buffer[index] |= (1 << bit); // Зажечь пиксель
  } else {
    oled_buffer[index] &= ~(1 << bit); // Погасить пиксель
  }
}

// Отрисовка прямоугольной рамки
static void oled_draw_rect(int x, int y, int w, int h, bool color) {
  // Горизонтальные линии
  for (int i = x; i < x + w; i++) {
    oled_draw_pixel(i, y, color);
    oled_draw_pixel(i, y + h - 1, color);
  }
  // Вертикальные линии
  for (int j = y; j < y + h; j++) {
    oled_draw_pixel(x, j, color);
    oled_draw_pixel(x + w - 1, j, color);
  }
}

// Отрисовка линии по алгоритму Брезенхэма
static void oled_draw_line(int x0, int y0, int x1, int y1, bool color) {
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy, e2;

  while (1) {
    oled_draw_pixel(x0, y0, color);
    if (x0 == x1 && y0 == y1)
      break;
    e2 = 2 * err;
    if (e2 >= dy) {
      err += dy;
      x0 += sx;
    }
    if (e2 <= dx) {
      err += dx;
      y0 += sy;
    }
  }
}

// Отрисовка постоянных (фоновых) элементов сцены
static void draw_background(void) {
  // Рамка по периметру
  oled_draw_rect(0, 0, 128, 64, true);

  // Диагональный крест
  oled_draw_line(0, 0, 127, 63, true);
  oled_draw_line(0, 63, 127, 0, true);

  // Центральный прямоугольник
  oled_draw_rect(44, 22, 40, 20, true);
}

// Отправка всего кадрового буфера в дисплей с гарантированным сбросом адреса
static void oled_update(i2c_master_dev_handle_t dev_handle) {
  // Команды сброса курсора записи в нулевые координаты (0, 0)
  // 0x21 - диапазон столбцов от 0 до 127
  // 0x22 - диапазон страниц от 0 до 7
  static const uint8_t reset_addr_cmds[] = {
      0x00,             // Control Byte: поток команд
      0x21, 0x00, 0x7F, // Set Column Address: start=0, end=127
      0x22, 0x00, 0x07  // Set Page Address: start=0, end=7
  };

  // 1. Принудительно возвращаем указатель памяти SSD1306 в начало координат
  i2c_master_transmit(dev_handle, reset_addr_cmds, sizeof(reset_addr_cmds),
                      100);

  // 2. Передаем сам кадровый буфер (байт 0x40 + 1024 байта пикселей)
  i2c_master_transmit(dev_handle, oled_buffer, sizeof(oled_buffer), 1000);
}

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

  // 4. Очищаем кадровый буфер и рисуем тестовую графику
  oled_buffer[0] = 0x40; // Префикс данных I2C
  oled_clear();

  // Рисуем рамку по внешнему периметру дисплея (128x64)
  oled_draw_rect(0, 0, 128, 64, true);

  // Рисуем диагональный крест через весь экран
  oled_draw_line(0, 0, 127, 63, true);
  oled_draw_line(0, 63, 127, 0, true);

  // Рисуем прямоугольник по центру
  oled_draw_rect(44, 22, 40, 20, true);

  // 5. Выгружаем нарисованный кадр в дисплей
  ESP_LOGI(TAG, "Отрисовка геометрии...");
  oled_update(dev_handle);

  // 6. Анимация: бегающий квадратик внутри экрана в цикле
  // Канонический цикл рендеринга: Clear -> Draw World -> Draw Actor -> Present
  ESP_LOGI(TAG, "Запуск чистого цикла рендеринга...");
  int box_x = 5;
  int box_dir = 1;

  while (1) {
    oled_fill();
    // Шаг 1: Очищаем кадровый буфер
    oled_clear();

    // Шаг 2: Рисуем статический фон (крест, рамку, центр)
    draw_background();

    // Шаг 3: Рассчитываем координаты и рисуем подвижный объект поверх
    box_x += box_dir * 2;
    if (box_x >= 112)
      box_dir = -1;
    if (box_x <= 5)
      box_dir = 1;

    oled_draw_rect(box_x, 50, 10, 10, true);

    // Шаг 4: Выталкиваем сформированный кадр на дисплей
    oled_update(dev_handle);

    // Задержка между кадрами (~50 FPS)
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}
