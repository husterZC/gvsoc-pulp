// SPDX-License-Identifier: Apache-2.0
// MXCoreFP4 calibration extension. E2M1, RNE, finite saturation, E8M0 scale.
module mxcore_fp4_quantizer #(
  parameter int unsigned BlockSize = 32,
  parameter int unsigned ScaleWidth = 8,
  parameter int unsigned InputDataWidth = 1024
) (
  input logic clk_i,
  input logic rst_ni,
  input logic block_poison_i,
  hwpe_stream_intf_stream.sink fp32_result_i,
  hwpe_stream_intf_stream.source mxfp8_result_o,
  hwpe_stream_intf_stream.source mx_result_scale_o
);
  // Port names preserve the upstream connection; data is packed FP4 here.
  localparam int unsigned Blocks = InputDataWidth / (BlockSize * 32);

  function automatic logic [31:0] threshold(input int index, input int scale);
    logic [31:0] base;
    logic [23:0] significand;
    int exponent;
    case (index)
      0: base = 32'h3e800000; // .25
      1: base = 32'h3f400000; // .75
      2: base = 32'h3fa00000; // 1.25
      3: base = 32'h3fe00000; // 1.75
      4: base = 32'h40200000; // 2.5
      5: base = 32'h40600000; // 3.5
      default: base = 32'h40a00000; // 5
    endcase
    exponent = int'(base[30:23]) + scale;
    significand = {1'b1, base[22:0]};
    if (exponent >= 255) return 32'h7f800000;
    if (exponent <= 0) return {8'b0, significand} >> (1-exponent);
    return (32'(exponent) << 23) | {9'b0, base[22:0]};
  endfunction

  for (genvar b = 0; b < Blocks; b++) begin : gen_block
    logic [30:0] maximum;
    logic poison;
    logic [7:0] scale;
    logic [31:0] value, midpoint;
    logic [2:0] code;
    always_comb begin
      maximum = '0;
      poison = 1'b0;
      for (int i = 0; i < BlockSize; i++) begin
        if (fp32_result_i.data[(b*BlockSize+i)*32+:31] > maximum)
          maximum = fp32_result_i.data[(b*BlockSize+i)*32+:31];
        if (fp32_result_i.data[(b*BlockSize+i)*32+23+:8] == 8'hff)
          poison = 1'b1;
      end
      // floor(log2(max_abs))-2, biased by 127. Subnormal scales saturate.
      if (poison) scale = 8'hff;
      else if (maximum == 0) scale = 8'd127;
      else if (maximum[30:23] < 2) scale = 0;
      else scale = maximum[30:23] - 8'd2;
      mx_result_scale_o.data[b*8+:8] = scale;
      value = '0;
      midpoint = '0;
      code = '0;
      for (int i = 0; i < BlockSize; i++) begin
        value = fp32_result_i.data[(b*BlockSize+i)*32+:32];
        code = 0;
        for (int j = 0; j < 7; j++) begin
          midpoint = threshold(j, int'(scale)-127);
          if ({1'b0, value[30:0]} > midpoint ||
              ({1'b0, value[30:0]} == midpoint && (j % 2 == 1)))
            code = 3'(j+1);
        end
        // E2M1 has no per-element NaN. A poisoned block has E8M0=255.
        mxfp8_result_o.data[(b*BlockSize+i)*4+:4] =
          poison ? 4'b0 : {value[31], code};
      end
    end
  end
  assign fp32_result_i.ready = mxfp8_result_o.ready;
  assign mxfp8_result_o.valid = fp32_result_i.valid;
  assign mxfp8_result_o.strb = '1;
  // Upstream scale FIFO gates acceptance with the result FIFO ready signal.
  assign mx_result_scale_o.valid = fp32_result_i.valid;
  assign mx_result_scale_o.strb = '1;
endmodule
