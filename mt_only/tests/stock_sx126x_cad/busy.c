/* Deterministic time and BUSY line for the production HAL wait routine. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>

/* Mirror Zephyr IS_ENABLED for an undefined option or literal 1. */
#define FLAG_1 ignored,
#define SECOND(first, second, ...) second
#define EXPAND(value) SECOND(value 1, 0)
#define LOOKUP(value) EXPAND(FLAG_##value)
#define IS_ENABLED(value) LOOKUP(value)
#define SX126X_BUSY_DEFAULT_TIMEOUT (IS_ENABLED(CONFIG_ZEPHCORE_DUAL_RX_BENCH) ? 200 : 1000)
#define LOG_WRN(...) ((void)0)
struct sx126x_data { bool config_valid; bool rx_duty_cycle_enabled; };
struct device { struct sx126x_data *data; };
static uint32_t elapsed, clear_at;
static unsigned faults, wakes;
static bool sx126x_hal_is_busy(const struct device *dev)
{
	(void)dev;
	return elapsed < clear_at;
}
static int sx126x_bench_fault(const struct device *dev, int reason)
{
	(void)dev;
	++faults;
	return reason;
}
static void sx126x_hal_wakeup(const struct device *dev)
{
	(void)dev;
	++wakes;
}
#define k_busy_wait(us) (elapsed += (us))
#define k_msleep(ms) (elapsed += (ms) * 1000U)
#define WAIT_FOR(condition, timeout, delay) ({                                \
	const uint32_t end = elapsed + (timeout);                             \
	while (!(condition) && elapsed < end) { delay; }                      \
	(condition);                                                         \
})
#include "busy.inc"
static void check(const struct device *dev, uint32_t limit, uint32_t release,
		  int status, uint32_t duration)
{
	elapsed = faults = wakes = 0;
	clear_at = release;
	assert(sx126x_hal_wait_busy(dev, limit) == status);
	assert(elapsed == duration);
	assert(faults == (unsigned)(status != 0));
	assert(wakes == 0);
}
int main(void)
{
	struct sx126x_data state = {0};
	const struct device dev = {&state};
	const uint32_t budget = SX126X_BUSY_DEFAULT_TIMEOUT;
	const uint32_t after = TEST_CAPPED ? 50 : budget;
	check(&dev, budget, 110000, 0, 110000);
	state.config_valid = true;
	check(&dev, budget, 130, 0, 130);
	check(&dev, budget, 49000, 0, 49000);
	check(&dev, budget, UINT32_MAX, -ETIMEDOUT, after * 1000U);
	check(&dev, budget, 51000, TEST_CAPPED ? -ETIMEDOUT : 0,
	      TEST_CAPPED ? 50000 : 51000);
	check(&dev, 18, 19000, -ETIMEDOUT, 18000);
	state.rx_duty_cycle_enabled = true;
	check(&dev, budget, 110000, 0, 110000);
	return 0;
}
