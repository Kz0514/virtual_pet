/**
 * MPU6500 DMP wrapper implementation.
 */
#include "dmp_mpu.h"
#include "board.h"
#include "mpu6500.h"
#include "dmp_port.h"
#include "inv_mpu.h"
#include "inv_mpu_dmp_motion_driver.h"
#include "esp_log.h"
#include <math.h>

static const char *TAG = "dmp";
i2c_master_dev_handle_t dmp_i2c_dev = NULL;

#define Q30 1073741824.0f

/* Cached latest angles + steps (updated by background task) */
static dmp_angles_t s_angles = {0};
static unsigned long s_steps = 0;
static bool s_data_ready = false;

/* Raw accel consumers (e.g. shake/tap detectors). Called from bg task context. */
static void (*volatile s_accel_cbs[DMP_MAX_ACCEL_CBS])(short ax, short ay, short az);

/* ── Flat-reference calibration ── */
static float s_q_ref[4] = {1.0f, 0.0f, 0.0f, 0.0f}; /* conjugate of flat ref */
static bool s_flat_cal = false;

/* Quaternion multiply: q = a * b (Hamilton convention) */
static inline void q_mult(const float *a, const float *b, float *q) {
 q[0] = a[0]*b[0] - a[1]*b[1] - a[2]*b[2] - a[3]*b[3];
 q[1] = a[0]*b[1] + a[1]*b[0] + a[2]*b[3] - a[3]*b[2];
 q[2] = a[0]*b[2] - a[1]*b[3] + a[2]*b[0] + a[3]*b[1];
 q[3] = a[0]*b[3] + a[1]*b[2] - a[2]*b[1] + a[3]*b[0];
}

static bool s_dmp_off = false; /* 息屏降频: 消费者 (shake/tap) 全 gate, 轮询 50→250ms 稀释到期点 */
static uint32_t s_off_poll_ms = 250; /* off 模式轮询间隔 (触摸深闲档可再拉长) */

void dmp_mpu_set_off(bool off)
{
    s_dmp_off = off;
}

/* 息屏 off 模式轮询间隔 (ms) — 触摸动态刷新率深闲档 (2Hz) 时调 500,
 * 快探档 (20Hz) 时调回 250。仅 off 模式生效, 亮屏恒 50ms。 */
void dmp_mpu_set_off_interval(uint32_t ms)
{
    s_off_poll_ms = ms;
}

static void dmp_bg_task(void *pv) {
 short gyro[3], accel[3], sensors;
 long quat[4];
 unsigned long ts;
 unsigned char more;
 while (1) {
 for (int i = 0; i < 8; i++) {
 if (dmp_read_fifo(gyro, accel, quat, &ts, &sensors, &more)) break;
 if (sensors & INV_WXYZ_QUAT) {
 float q0 = quat[0] / Q30, q1 = quat[1] / Q30;
 float q2 = quat[2] / Q30, q3 = quat[3] / Q30;

 /* Apply flat-reference correction if calibrated */
 if (s_flat_cal) {
 float c[4];
 q_mult(s_q_ref, (float[]){q0, q1, q2, q3}, c);
 q0 = c[0]; q1 = c[1]; q2 = c[2]; q3 = c[3];
 }

 s_angles.pitch = asinf(-2.0f*q1*q3 + 2.0f*q0*q2) * 57.3f;
 s_angles.roll = atan2f(2.0f*q2*q3 + 2.0f*q0*q1,
 -2.0f*q1*q1 - 2.0f*q2*q2 + 1.0f) * 57.3f;
 s_angles.yaw = atan2f(2.0f*(q0*q3 + q1*q2),
 q0*q0 + q1*q1 - q2*q2 - q3*q3) * 57.3f;
 if (s_angles.yaw < 0) s_angles.yaw += 360.0f;
 s_data_ready = true;
 }
 /* Forward raw accel to consumers (shake/tap detectors etc.) */
 if (sensors & INV_XYZ_ACCEL) {
 for (int i = 0; i < DMP_MAX_ACCEL_CBS; i++) {
 if (s_accel_cbs[i])
 s_accel_cbs[i](accel[0], accel[1], accel[2]);
 }
 }
 if (!more) break;
 }
 dmp_get_pedometer_step_count(&s_steps);
 vTaskDelay(s_dmp_off ? pdMS_TO_TICKS(s_off_poll_ms) : pdMS_TO_TICKS(50)); /* 息屏降频 (跟随触摸深闲档) / 亮屏 20Hz */
 }
}

#define DEFAULT_MPU_HZ 100

/* Orientation matrix from reference (negate X,Y = 6050-style).
 * This was the matrix used in the best-performing test. */
static signed char gyro_orientation[9] = {-1,0,0, 0,-1,0, 0,0,1};

static unsigned short inv_row_2_scale(const signed char *row) {
 unsigned short b;
 if (row[0] > 0) b = 0;
 else if (row[0] < 0) b = 4;
 else if (row[1] > 0) b = 1;
 else if (row[1] < 0) b = 5;
 else if (row[2] > 0) b = 2;
 else if (row[2] < 0) b = 6;
 else b = 7;
 return b;
}

static unsigned short inv_orientation_matrix_to_scalar(const signed char *mtx) {
 unsigned short scalar;
 scalar = inv_row_2_scale(mtx);
 scalar |= inv_row_2_scale(mtx + 3) << 3;
 scalar |= inv_row_2_scale(mtx + 6) << 6;
 return scalar;
}

static int run_self_test(void) {
 int result;
 long gyro[3], accel[3];
 result = mpu_run_6500_self_test(gyro, accel, 0);
 if (result == 0x07) {
 float sens;
 unsigned short accel_sens;
 mpu_get_gyro_sens(&sens);
 gyro[0] = (long)(gyro[0] * sens);
 gyro[1] = (long)(gyro[1] * sens);
 gyro[2] = (long)(gyro[2] * sens);
 dmp_set_gyro_bias(gyro);
 mpu_get_accel_sens(&accel_sens);
 accel[0] *= accel_sens;
 accel[1] *= accel_sens;
 accel[2] *= accel_sens;
 dmp_set_accel_bias(accel);
 return 0;
 }
 return -1;
}

/* ════════════════════════════════════════════════════════════════ */

esp_err_t dmp_mpu_init(void)
{
 ESP_LOGI(TAG, "DMP init…");

 /* 1. Reuse mpu6500's I2C handle */
 dmp_i2c_dev = mpu6500_get_i2c_dev();
 if (!dmp_i2c_dev) { ESP_LOGE(TAG, "I2C dev not ready"); return ESP_FAIL; }

 /* 2. Init MPU (sets ±2000dps gyro, ±2g accel) */
 struct int_param_s int_param = {0};
 if (mpu_init(&int_param) != 0) { ESP_LOGE(TAG, "mpu_init fail"); return ESP_FAIL; }

 /* Restore accel DLPF (mpu_init bypasses it with FCHOICE_B=1) */
 {
 uint8_t acfg2 = 0x40 | 0x04; /* FIFO_SIZE=1K | FCHOICE_B=0 | DLPF=20Hz */
 i2c_master_transmit(dmp_i2c_dev, (uint8_t[]){0x1D, acfg2}, 2, 10);
 }

 /* 3. Config */
 if (mpu_set_sensors(INV_XYZ_GYRO | INV_XYZ_ACCEL)) { ESP_LOGE(TAG, "sensors fail"); return ESP_FAIL; }
 if (mpu_configure_fifo(INV_XYZ_GYRO | INV_XYZ_ACCEL)) { ESP_LOGE(TAG, "fifo fail"); return ESP_FAIL; }
 if (mpu_set_sample_rate(DEFAULT_MPU_HZ)) { ESP_LOGE(TAG, "rate fail"); return ESP_FAIL; }

 /* 4. Load DMP firmware */
 if (dmp_load_motion_driver_firmware()) { ESP_LOGE(TAG, "DMP fw load fail"); return ESP_FAIL; }
 ESP_LOGI(TAG, "DMP firmware loaded");

 /* 5. Orientation */
 dmp_set_orientation(inv_orientation_matrix_to_scalar(gyro_orientation));

 /* 6. Enable features */
 if (dmp_enable_feature(DMP_FEATURE_6X_LP_QUAT | DMP_FEATURE_PEDOMETER |
 DMP_FEATURE_TAP | DMP_FEATURE_ANDROID_ORIENT |
 DMP_FEATURE_SEND_RAW_ACCEL | DMP_FEATURE_SEND_CAL_GYRO |
 DMP_FEATURE_GYRO_CAL)) {
 ESP_LOGE(TAG, "feature fail"); return ESP_FAIL;
 }

 /* 7. DMP output rate — 40Hz: 敲击冲击峰(5-15ms)被 20Hz 采样稀释严重, 提到 40Hz 减半相位误差 */
 if (dmp_set_fifo_rate(40)) { ESP_LOGE(TAG, "fifo rate fail"); return ESP_FAIL; }

 /* 8. Enable DMP */
 if (mpu_set_dmp_state(1)) { ESP_LOGE(TAG, "DMP state fail"); return ESP_FAIL; }

 /* 9. Self-test → if fails, manual calibration */
 if (run_self_test()) {
 ESP_LOGW(TAG, "Self-test failed, measuring gyro bias (hold still)...");
 long gyro_bias[3] = {0, 0, 0};
 short gyro[3], accel[3], sensors;
 long quat[4]; unsigned long ts; unsigned char more;
 vTaskDelay(pdMS_TO_TICKS(500));
 int n = 0;
 for (int i = 0; i < 100; i++) {
 if (!dmp_read_fifo(gyro, accel, quat, &ts, &sensors, &more)) {
 gyro_bias[0] += gyro[0]; gyro_bias[1] += gyro[1]; gyro_bias[2] += gyro[2];
 n++;
 }
 vTaskDelay(pdMS_TO_TICKS(10));
 }
 if (n > 0) {
 gyro_bias[0] /= n; gyro_bias[1] /= n; gyro_bias[2] /= n;
 float sens; mpu_get_gyro_sens(&sens);
 gyro_bias[0] = (long)(gyro_bias[0] * sens);
 gyro_bias[1] = (long)(gyro_bias[1] * sens);
 gyro_bias[2] = (long)(gyro_bias[2] * sens);
 dmp_set_gyro_bias(gyro_bias);
 ESP_LOGI(TAG, "Gyro bias: %ld %ld %ld (sens=%.4f, n=%d)",
 gyro_bias[0], gyro_bias[1], gyro_bias[2], sens, n);

 long accel_bias[3] = {0, 0, 0};
 int an = 0;
 for (int i = 0; i < 100; i++) {
 if (!dmp_read_fifo(gyro, accel, quat, &ts, &sensors, &more)) {
 accel_bias[0] += accel[0]; accel_bias[1] += accel[1]; accel_bias[2] += accel[2];
 an++;
 }
 vTaskDelay(pdMS_TO_TICKS(10));
 }
 if (an > 0) {
 accel_bias[0] /= an; accel_bias[1] /= an; accel_bias[2] /= an;
 accel_bias[2] -= 16384;
 dmp_set_accel_bias(accel_bias);
 ESP_LOGI(TAG, "Accel bias: %ld %ld %ld (n=%d)",
 accel_bias[0], accel_bias[1], accel_bias[2], an);
 }
 }
 }

 /* 10. Start background FIFO drain task */
 /* : 回退内部栈 — dmp_bg 每 5ms 高频跑, PSRAM 栈撞 flash 写
 * 冻结窗口概率高 (双异常教训) */
 xTaskCreate(dmp_bg_task, "dmp_bg", 3072, NULL, 5, NULL);
 ESP_LOGI(TAG, "DMP ready. Keep device flat & still for warm-up ...");

 /* 11. Delayed recalibration after 5s sensor warm-up */
 vTaskDelay(pdMS_TO_TICKS(5000));
 {
 long gb[3]={0,0,0}, ab[3]={0,0,0};
 short g[3], a[3], s; long q[4]; unsigned long t; unsigned char m;
 int gn=0, an=0;
 for (int i = 0; i < 100; i++) {
 if (!dmp_read_fifo(g, a, q, &t, &s, &m)) {
 gb[0]+=g[0]; gb[1]+=g[1]; gb[2]+=g[2]; gn++;
 ab[0]+=a[0]; ab[1]+=a[1]; ab[2]+=a[2]; an++;
 }
 vTaskDelay(pdMS_TO_TICKS(10));
 }
 if (gn>0 && an>0) {
 gb[0]/=gn; gb[1]/=gn; gb[2]/=gn;
 ab[0]/=an; ab[1]/=an; ab[2]/=an; ab[2]-=16384;
 float ss; mpu_get_gyro_sens(&ss);
 gb[0]=(long)(gb[0]*ss); gb[1]=(long)(gb[1]*ss); gb[2]=(long)(gb[2]*ss);
 dmp_set_gyro_bias(gb); dmp_set_accel_bias(ab);
 ESP_LOGI(TAG,"Warm recal: gyro=%ld %ld %ld acc=%ld %ld %ld",
 gb[0],gb[1],gb[2],ab[0],ab[1],ab[2]);
 }
 }
 ESP_LOGI(TAG, "Calibration done");

 /* 12. Capture flat-reference quaternion */
 {
 float q_sum[4] = {0};
 short g[3], a[3], s; long q[4]; unsigned long t; unsigned char m;
 int qn = 0;
 for (int i = 0; i < 50; i++) {
 if (!dmp_read_fifo(g, a, q, &t, &s, &m)) {
 if (s & INV_WXYZ_QUAT) {
 q_sum[0] += q[0] / Q30; q_sum[1] += q[1] / Q30;
 q_sum[2] += q[2] / Q30; q_sum[3] += q[3] / Q30; qn++;
 }
 }
 vTaskDelay(pdMS_TO_TICKS(20));
 }
 if (qn > 10) {
 q_sum[0] /= qn; q_sum[1] /= qn; q_sum[2] /= qn; q_sum[3] /= qn;
 float mag = sqrtf(q_sum[0]*q_sum[0] + q_sum[1]*q_sum[1] +
 q_sum[2]*q_sum[2] + q_sum[3]*q_sum[3]);
 q_sum[0] /= mag; q_sum[1] /= mag; q_sum[2] /= mag; q_sum[3] /= mag;

 s_q_ref[0] = q_sum[0];
 s_q_ref[1] = -q_sum[1];
 s_q_ref[2] = -q_sum[2];
 s_q_ref[3] = -q_sum[3];
 s_flat_cal = true;

 float raw_pitch = asinf(-2.0f*q_sum[1]*q_sum[3] + 2.0f*q_sum[0]*q_sum[2]) * 57.3f;
 float raw_roll = atan2f(2.0f*q_sum[2]*q_sum[3] + 2.0f*q_sum[0]*q_sum[1],
 -2.0f*q_sum[1]*q_sum[1] - 2.0f*q_sum[2]*q_sum[2] + 1.0f) * 57.3f;
 ESP_LOGI(TAG, "Flat ref: q=(%.4f,%.4f,%.4f,%.4f) raw_pitch=%.1f raw_roll=%.1f → cal'd to 0,0",
 q_sum[0], q_sum[1], q_sum[2], q_sum[3], raw_pitch, raw_roll);
 } else {
 ESP_LOGW(TAG, "Flat ref capture failed (qn=%d)", qn);
 }
 }

 ESP_LOGI(TAG, "Ready.");
 return ESP_OK;
}

/* ════════════════════════════════════════════════════════════════ */

esp_err_t dmp_mpu_get_angles(dmp_angles_t *angles)
{
 if (!angles || !s_data_ready) return ESP_FAIL;
 *angles = s_angles;
 return ESP_OK;
}

unsigned long dmp_mpu_get_steps(void) { return s_steps; }

void dmp_mpu_reset_steps(void) {
 dmp_set_pedometer_step_count(0);
}

/* ══════════ Tap & Orientation callbacks ══════ */

void dmp_mpu_register_tap_cb(void (*cb)(unsigned char direction, unsigned char count))
{
 dmp_register_tap_cb(cb);
}

void dmp_mpu_register_orient_cb(void (*cb)(unsigned char orientation))
{
 dmp_register_android_orient_cb(cb);
}

void dmp_mpu_add_accel_cb(void (*cb)(short ax, short ay, short az))
{
 for (int i = 0; i < DMP_MAX_ACCEL_CBS; i++) {
 if (s_accel_cbs[i] == cb) return; /* 幂等 */
 if (!s_accel_cbs[i]) { s_accel_cbs[i] = cb; return; }
 }
}
