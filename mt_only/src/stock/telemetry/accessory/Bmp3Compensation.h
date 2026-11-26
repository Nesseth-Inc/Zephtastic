// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2020 Bosch Sensortec GmbH. See docs/licenses/Bosch-BMP3.txt.
// Floating-point compensation functions from Adafruit_BMP3XX 2.1.6 bmp3.c.
#pragma once
#include <stdint.h>
namespace mt::stock::telemetry::accessory::bmp3math {
struct bmp3_uncomp_data { uint32_t pressure,temperature; };
struct bmp3_quantized_calib_data {
    double par_t1,par_t2,par_t3,par_p1,par_p2,par_p3,par_p4,par_p5,par_p6,par_p7,par_p8,par_p9,par_p10,par_p11,t_lin;
};
struct bmp3_calib_data {bmp3_quantized_calib_data quantized_calib_data;};
inline float pow_bmp3(double base,uint8_t power);
inline double compensate_temperature(const struct bmp3_uncomp_data *uncomp_data, struct bmp3_calib_data *calib_data)
{
    uint32_t uncomp_temp = uncomp_data->temperature;
    double partial_data1;
    double partial_data2;

    partial_data1 = (double)(uncomp_temp - calib_data->quantized_calib_data.par_t1);
    partial_data2 = (double)(partial_data1 * calib_data->quantized_calib_data.par_t2);

    /* Update the compensated temperature in calib structure since this is
     * needed for pressure calculation */
    calib_data->quantized_calib_data.t_lin = partial_data2 + (partial_data1 * partial_data1) *
                                             calib_data->quantized_calib_data.par_t3;

    /* Returns compensated temperature */
    return calib_data->quantized_calib_data.t_lin;
}

/*!
 * @brief This internal API is used to compensate the raw pressure data and
 * return the compensated pressure data in double data type.
 * For e.g. returns pressure in Pascal p = 95305.295 which is 953.05295 hecto pascal
 */
inline double compensate_pressure(const struct bmp3_uncomp_data *uncomp_data, const struct bmp3_calib_data *calib_data)
{
    const struct bmp3_quantized_calib_data *quantized_calib_data = &calib_data->quantized_calib_data;

    /* Variable to store the compensated pressure */
    double comp_press;

    /* Temporary variables used for compensation */
    double partial_data1;
    double partial_data2;
    double partial_data3;
    double partial_data4;
    double partial_out1;
    double partial_out2;

    partial_data1 = quantized_calib_data->par_p6 * quantized_calib_data->t_lin;
    partial_data2 = quantized_calib_data->par_p7 * pow_bmp3(quantized_calib_data->t_lin, 2);
    partial_data3 = quantized_calib_data->par_p8 * pow_bmp3(quantized_calib_data->t_lin, 3);
    partial_out1 = quantized_calib_data->par_p5 + partial_data1 + partial_data2 + partial_data3;
    partial_data1 = quantized_calib_data->par_p2 * quantized_calib_data->t_lin;
    partial_data2 = quantized_calib_data->par_p3 * pow_bmp3(quantized_calib_data->t_lin, 2);
    partial_data3 = quantized_calib_data->par_p4 * pow_bmp3(quantized_calib_data->t_lin, 3);
    partial_out2 = uncomp_data->pressure *
                   (quantized_calib_data->par_p1 + partial_data1 + partial_data2 + partial_data3);
    partial_data1 = pow_bmp3((double)uncomp_data->pressure, 2);
    partial_data2 = quantized_calib_data->par_p9 + quantized_calib_data->par_p10 * quantized_calib_data->t_lin;
    partial_data3 = partial_data1 * partial_data2;
    partial_data4 = partial_data3 + pow_bmp3((double)uncomp_data->pressure, 3) * quantized_calib_data->par_p11;
    comp_press = partial_out1 + partial_out2 + partial_data4;

    return comp_press;
}

/*!
 * @brief This internal API is used to calculate the power functionality for
 *  floating point values.
 */
inline float pow_bmp3(double base, uint8_t power)
{
    float pow_output = 1;

    while (power != 0)
    {
        pow_output = (float) base * pow_output;
        power--;
    }

    return pow_output;
}
}
