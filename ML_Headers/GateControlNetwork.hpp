// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[ML_Headers/GateControlNetwork.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [BINARY_FP]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the original GCN sketch — a tiny fixed-point MLP (forward + backward pass) for gate control; exploratory-era module]
// [CONTAINS]
//   - [STRUCT]_[GCN_input]
//   - [STRUCT]_[GCN_network]
//   - [FUNCTION]_[GCN_forward]
//   - [FUNCTION]_[GCN_backward]
//======================================================================================================
// This is going to just control the gate conditions, basically the watcher module i referenced earlier, im not sure how to actually implement this yet or everything it needs but i figure going ahead and sketching i tout will work,
//
//------------------------------------------------------------------------------
// [SECTION]_[INCLUDE]
//------------------------------------------------------------------------------
#ifndef GATE_CONTROL_NETWORK_HPP
#define GATE_CONTROL_NETWORK_HPP

#include "../FixedPoint/FixedPointN.hpp"
#include "LinearRegression3X.hpp"
#include "ROR_regressor.hpp"
//======================================================================
// [STRUCT]_[GCN_input]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [BINARY_FP]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the 6 fixed-point MLP inputs (volume/price/portfolio/slope/…) + operator[] for offset-indexed access]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F> struct GCN_input {
    FPN_Binary<F> volume;
    FPN_Binary<F> price;
    FPN_Binary<F> portfolio_value;
    FPN_Binary<F> portolio_delta;
    FPN_Binary<F> slope;
    FPN_Binary<F> slope_of_slopes;
    FPN_Binary<F> &operator[](unsigned i) { return (&volume)[i]; }
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]   (tool-refreshed — do NOT hand-edit; check_cache_layout --fix owns these)
//======================================================================
// [END_STRUCT]_[GCN_input]
//======================================================================

// [ASSERT]_[LAYOUT_LOCK]_[sizeof(GCN_input<64>) == 6 * sizeof(FPN_Binary<64>)]
// [WHY]_[the 6 inputs are contiguous FPN_Binary — operator[] indexes them by offset from &volume]
static_assert(sizeof(GCN_input<64>) == 6 * sizeof(FPN_Binary<64>), "GCN_input size mismatch");

//======================================================================
// [STRUCT]_[GCN_network]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [BINARY_FP]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the MLP weights + biases — template-sized hidden/output layers as flat 1D arrays (2D-grid indexing)]
//======================================================================
// [CODE]
//======================================================================
template <unsigned F, unsigned INPUTS, unsigned HIDDEN, unsigned OUTPUTS> struct GCN_network {
    FPN_Binary<F> w_hidden[INPUTS * HIDDEN];
    FPN_Binary<F> b_hidden[HIDDEN];
    FPN_Binary<F> hidden_out[HIDDEN];

    FPN_Binary<F> w_output[HIDDEN * OUTPUTS];
    FPN_Binary<F> b_output[OUTPUTS];
    FPN_Binary<F> output[OUTPUTS];
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]   (tool-refreshed — do NOT hand-edit; check_cache_layout --fix owns these)
//======================================================================
// [END_STRUCT]_[GCN_network]
//======================================================================

//======================================================================
// [FUNCTION]_[GCN_forward]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [BINARY_FP]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the MLP forward pass — bias + weighted-sum + ReLU per hidden neuron, then hidden->output]
//======================================================================
// [CODE]
//======================================================================
//im gonna go back through these and make them branchless this is just boilerplate lol
//
//this is basically doing a standard forward pass where or each hidden nueron, it starts with the bias, then loops through every input and multiplies it be the weight connecting it to the hidden nuerion, and accumulates the result, then it applies ReLU, and does the same thing from hidden layer to output layer
//
//the weight indexing is how you layout a 2D grid in a 1D array, so you have inputs going to hidden nuerons, and its just a grid of inputs * hidden weights, i wish i actually understood this stuff like on a deeper leve than conceptual, its really interesting but idk, maybe im just stupid, i have ZERO clue why people are cloning my stuff, like if your doing it to make me feel better thanks i guess, it kind of works until it doesnt
template <unsigned F, unsigned INPUTS, unsigned HIDDEN, unsigned OUTPUTS>
void GCN_forward(GCN_network<F, INPUTS, HIDDEN, OUTPUTS> &net, GCN_input<F> &input) {
    // Compute hidden layer
    for (unsigned i = 0; i < HIDDEN; ++i) {
        net.hidden_out[i] = net.b_hidden[i];
        for (unsigned j = 0; j < INPUTS; ++j) {
            net.hidden_out[i] = FPN_Add(net.hidden_out[i], FPN_Mul(net.w_hidden[i * INPUTS + j], input[j]));
        }
        // Apply activation function (e.g., ReLU)
        FPN_Binary<F> zero   = FPN_Zero<F>();
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
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[GCN_forward]
//======================================================================

//======================================================================
// [FUNCTION]_[GCN_backward]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [BINARY_FP]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the MLP backward pass — output error -> backprop through the weights -> nudge by learning_rate]
//======================================================================
// [CODE]
//======================================================================
//this apparently is just the if you know what the output was, and you know what you wanted it to be, you have the error difference, and then you push that back through it to figure out how much each weight contrinbuted to the error, and then you can nudge them in the opposite direction, by the learning rate
template <unsigned F, unsigned INPUTS, unsigned HIDDEN, unsigned OUTPUTS>
void GCN_backward(GCN_network<F, INPUTS, HIDDEN, OUTPUTS> &net, GCN_input<F> &input, FPN_Binary<F> &target, FPN_Binary<F> learning_rate) {
    // Compute output layer error
    FPN_Binary<F> output_error[OUTPUTS];
    for (unsigned i = 0; i < OUTPUTS; ++i) {
        output_error[i] = FPN_Sub(net.output[i], target); // error = output - target
    }

    // Compute hidden layer error and update weights/biases
    for (unsigned i = 0; i < HIDDEN; ++i) {
        FPN_Binary<F> hidden_error = FPN_Zero<F>();
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
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[GCN_backward]
//======================================================================
#endif
