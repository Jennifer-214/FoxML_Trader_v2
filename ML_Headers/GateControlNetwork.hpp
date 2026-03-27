// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [GATE CONTROL NETWORK]
//======================================================================================================
// This is going to just control the gate conditions, basically the watcher module i referenced earlier, im not sure how to actually implement this yet or everything it needs but i figure going ahead and sketching i tout will work,
//
//======================================================================================================
// [INCLUDE]
//======================================================================================================
#ifndef GATE_CONTROL_NETWORK_HPP
#define GATE_CONTROL_NETWORK_HPP

#include "../FixedPoint/FixedPointN.hpp"
#include "LinearRegression3X.hpp"
#include "ROR_regressor.hpp"
//======================================================================================================
// [STRUCTS]
//======================================================================================================
template <unsigned F> struct GCN_input {
    FPN<F> volume;
    FPN<F> price;
    FPN<F> portfolio_value;
    FPN<F> portolio_delta;
    FPN<F> slope;
    FPN<F> slope_of_slopes;
    FPN<F> &operator[](unsigned i) { return (&volume)[i]; }
};
static_assert(sizeof(GCN_input<64>) == 6 * sizeof(FPN<64>), "GCN_input size mismatch");

template <unsigned F, unsigned INPUTS, unsigned HIDDEN, unsigned OUTPUTS> struct GCN_network {
    FPN<F> w_hidden[INPUTS * HIDDEN];
    FPN<F> b_hidden[HIDDEN];
    FPN<F> hidden_out[HIDDEN];

    FPN<F> w_output[HIDDEN * OUTPUTS];
    FPN<F> b_output[OUTPUTS];
    FPN<F> output[OUTPUTS];
};

//======================================================================================================
// [FUNCTION]
//======================================================================================================
// [FORWARD PASS]
//======================================================================================================
//im gonna go back through these and make them branchless this is just boilerplate lol
//
//this is basically doing a standard forward pass where or each hidden nueron, it starts with the bias, then loops through every input and multiplies it be the weight connecting it to the hidden nuerion, and accumulates the result, then it applies ReLU, and does the same thing from hidden layer to output layer
//
//the weight indexing is how you layout a 2D grid in a 1D array, so you have inputs going to hidden nuerons, and its just a grid of inputs * hidden weights, i wish i actually understood this stuff like on a deeper leve than conceptual, its really interesting but idk, maybe im just stupid, i have ZERO clue why people are cloning my stuff, like if your doing it to make me feel better thanks i guess, it kind of works until it doesnt
//======================================================================================================
template <unsigned F, unsigned INPUTS, unsigned HIDDEN, unsigned OUTPUTS>
void GCN_forward(GCN_network<F, INPUTS, HIDDEN, OUTPUTS> &net, GCN_input<F> &input) {
    // Compute hidden layer
    for (unsigned i = 0; i < HIDDEN; ++i) {
        net.hidden_out[i] = net.b_hidden[i];
        for (unsigned j = 0; j < INPUTS; ++j) {
            net.hidden_out[i] = FPN_Add(net.hidden_out[i], FPN_Mul(net.w_hidden[i * INPUTS + j], input[j]));
        }
        // Apply activation function (e.g., ReLU)
        FPN<F> zero   = FPN_Zero<F>();
        net.hidden_out[i] = FPN_Max(net.hidden_out[i], zero);
    }

    // Compute output layer
    for (unsigned i = 0; i < OUTPUTS; ++i) {
        net.output[i] = net.b_output[i];
        for (unsigned j = 0; j < HIDDEN; ++j) {
            net.output[i] = FPN_Add(net.output[i], FPN_Mul(net.w_output[j * OUTPUTS + i], net.hidden_out[j]));
        }
        // No activation function on output layer for regression tasks
    }
}

//======================================================================================================
// [BACKWARD PASS]
//======================================================================================================
//this apparently is just the if you know what the output was, and you know what you wanted it to be, you have the error difference, and then you push that back through it to figure out how much each weight contrinbuted to the error, and then you can nudge them in the opposite direction, by the learning rate
//======================================================================================================
template <unsigned F, unsigned INPUTS, unsigned HIDDEN, unsigned OUTPUTS>
void GCN_backward(GCN_network<F, INPUTS, HIDDEN, OUTPUTS> &net, GCN_input<F> &input, FPN<F> &target, FPN<F> learning_rate) {
    // Compute output layer error
    FPN<F> output_error[OUTPUTS];
    for (unsigned i = 0; i < OUTPUTS; ++i) {
        output_error[i] = FPN_Sub(net.output[i], target); // error = output - target
    }

    // Compute hidden layer error and update weights/biases
    for (unsigned i = 0; i < HIDDEN; ++i) {
        FPN<F> hidden_error = FPN_Zero<F>();
        for (unsigned j = 0; j < OUTPUTS; ++j) {
            hidden_error = FPN_Add(hidden_error, FPN_Mul(net.w_output[i * OUTPUTS + j], output_error[j]));
            // Update output weights and biases
            net.w_output[i * OUTPUTS + j] =
                FPN_Sub(net.w_output[i * OUTPUTS + j], FPN_Mul(learning_rate, FPN_Mul(output_error[j], net.hidden_out[i])));
        }
        // Update hidden biases
        net.b_hidden[i] = FPN_Sub(net.b_hidden[i], FPN_Mul(learning_rate, hidden_error));
        // Update hidden weights
        for (unsigned k = 0; k < INPUTS; ++k) {
            net.w_hidden[k * HIDDEN + i] =
                FPN_Sub(net.w_hidden[k * HIDDEN + i], FPN_Mul(learning_rate, FPN_Mul(hidden_error, input[k])));
        }
    }
}
//======================================================================================================
//======================================================================================================
//======================================================================================================
//======================================================================================================
//======================================================================================================
//======================================================================================================
//======================================================================================================
//======================================================================================================
#endif
