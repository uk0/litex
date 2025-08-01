#
# This file is part of LiteX.
#
# Copyright (c) 2016-2017 Sebastien Bourdeauducq <sb@m-labs.hk>
# Copyright (c) 2019-2020 Florent Kermarrec <florent@enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

"""
IBM's 8b/10b Encoding

This scheme is used by a large number of protocols including Display Port, PCI
Express, Gigabit Ethernet, SATA and USB 3.

The encoding is built by combining an 5b/6b and 3b/4b encoding schemes and
guarantees both DC balance and enough bit transitions to recover the clock
signal.

Note: This encoding is *not* used by DVI/HDMI (that uses a *different* 8b/10b
scheme called TMDS).
"""

from migen import *

from litex.gen import *

from litex.soc.interconnect import stream

# Helpers ------------------------------------------------------------------------------------------

def K(x, y):
    return (y << 5) | x

def D(x, y):
    return (y << 5) | x

def disparity(word, nbits):
    n0 = 0
    n1 = 0
    for i in range(nbits):
        if word & (1 << i):
            n1 += 1
        else:
            n0 += 1
    return n1 - n0


def reverse_table_flip(inputs, flips, nbits):
    outputs = [None]*2**nbits

    for i, (word, flip) in enumerate(zip(inputs, flips)):
        if outputs[word] is not None:
            raise ValueError
        outputs[word] = i
        if flip:
            word_n = ~word & (2**nbits-1)
            if outputs[word_n] is not None:
                raise ValueError
            outputs[word_n] = i

    for i in range(len(outputs)):
        if outputs[i] is None:
            outputs[i] = 0

    return outputs


def reverse_table(inputs, nbits):
    outputs = [None]*2**nbits
    for i, word in enumerate(inputs):
        if outputs[word] is not None:
            raise ValueError
        outputs[word] = i
    for i in range(len(outputs)):
        if outputs[i] is None:
            outputs[i] = 0
    return outputs


# 5b6b ---------------------------------------------------------------------------------------------

table_5b6b = [
    0b011000,
    0b100010,
    0b010010,
    0b110001,
    0b001010,
    0b101001,
    0b011001,
    0b000111,
    0b000110,
    0b100101,
    0b010101,
    0b110100,
    0b001101,
    0b101100,
    0b011100,
    0b101000,
    0b100100,
    0b100011,
    0b010011,
    0b110010,
    0b001011,
    0b101010,
    0b011010,
    0b000101,
    0b001100,
    0b100110,
    0b010110,
    0b001001,
    0b001110,
    0b010001,
    0b100001,
    0b010100,
]
table_5b6b_unbalanced = [bool(disparity(c, 6)) for c in table_5b6b]
table_5b6b_flip       = list(table_5b6b_unbalanced)
table_5b6b_flip[7]    = True

table_6b5b = reverse_table_flip(table_5b6b, table_5b6b_flip, 6)

table_6b5b[0b001111] = 0b11100 # K.28
table_6b5b[0b110000] = 0b11100 # K.28

# 3b4b ---------------------------------------------------------------------------------------------

table_3b4b = [
    0b0100,
    0b1001,
    0b0101,
    0b0011,
    0b0010,
    0b1010,
    0b0110,
    0b0001,  # Primary D.x.7
]
table_3b4b_unbalanced = [bool(disparity(c, 4)) for c in table_3b4b]
table_3b4b_flip       = list(table_3b4b_unbalanced)
table_3b4b_flip[3]    = True

table_4b3b = reverse_table_flip(table_3b4b, table_3b4b_flip, 4)
# Alternative D.x.7
table_4b3b[0b0111] = 0b0111
table_4b3b[0b1000] = 0b0111

table_4b3b_kn = reverse_table(table_3b4b, 4)
table_4b3b_kp = reverse_table([~x & 0b1111 for x in table_3b4b], 4)
# Primary D.x.7 is not used
table_4b3b_kn[0b0001] = 0b000
table_4b3b_kn[0b1000] = 0b111
table_4b3b_kp[0b1110] = 0b000
table_4b3b_kp[0b0111] = 0b111

# Single Encoder -----------------------------------------------------------------------------------

@CEInserter()
class SingleEncoder(LiteXModule):
    def __init__(self, lsb_first=False):
        self.d        = Signal(8)
        self.k        = Signal()
        self.disp_in  = Signal()

        self.output   = Signal(10)
        self.disp_out = Signal()

        # # #

        # Stage 1: 5b/6b and 3b/4b encoding.
        code5b            = self.d[:5]
        code6b            = Signal(6, reset_less=True)
        code6b_unbalanced = Signal(reset_less=True)
        code6b_flip       = Signal()
        self.sync += [
            If(self.k & (code5b == 28),
                code6b.eq(0b110000),
                code6b_unbalanced.eq(1),
                code6b_flip.eq(1)
            ).Else(
                code6b.eq(Array(table_5b6b)[code5b]),
                code6b_unbalanced.eq(Array(table_5b6b_unbalanced)[code5b]),
                code6b_flip.eq(Array(table_5b6b_flip)[code5b])
            )
        ]

        code3b            = self.d[5:]
        code4b            = Signal(4, reset_less=True)
        code4b_unbalanced = Signal(reset_less=True)
        code4b_flip       = Signal()
        self.sync += [
            code4b.eq(Array(table_3b4b)[code3b]),
            code4b_unbalanced.eq(Array(table_3b4b_unbalanced)[code3b]),
            If(self.k,
                code4b_flip.eq(1)
            ).Else(
                code4b_flip.eq(Array(table_3b4b_flip)[code3b])
            )
        ]

        alt7_rd0 = Signal(reset_less=True)  # If disparity is -1, use alternative D.x.7.
        alt7_rd1 = Signal(reset_less=True)  # If disparity is +1, use alternative D.x.7.
        self.sync += [
            alt7_rd0.eq(0),
            alt7_rd1.eq(0),
            If(code3b == 7,
                If((code5b == 17) | (code5b == 18) | (code5b == 20),
                    alt7_rd0.eq(1)),
                If((code5b == 11) | (code5b == 13) | (code5b == 14),
                    alt7_rd1.eq(1)),
                If(self.k,
                    alt7_rd0.eq(1),
                    alt7_rd1.eq(1)
                )
            )
        ]

        # Stage 2 (combinatorial): disparity control.
        output_6b  = Signal(6)
        disp_inter = Signal()
        self.comb += [
            disp_inter.eq(self.disp_in ^ code6b_unbalanced),
            If(~self.disp_in & code6b_flip,
                output_6b.eq(~code6b)
            ).Else(
                output_6b.eq(code6b)
            )
        ]

        output_4b = Signal(4)
        self.comb += [
            If(~disp_inter & alt7_rd0,
                self.disp_out.eq(~disp_inter),
                output_4b.eq(0b0111)
            ).Elif(disp_inter & alt7_rd1,
                self.disp_out.eq(~disp_inter),
                output_4b.eq(0b1000)
            ).Else(
                self.disp_out.eq(disp_inter ^ code4b_unbalanced),
                If(~disp_inter & code4b_flip,
                    output_4b.eq(~code4b)
                ).Else(
                    output_4b.eq(code4b)
                )
            )
        ]

        output_msb_first = Signal(10)
        self.comb += output_msb_first.eq(Cat(output_4b, output_6b))
        if lsb_first:
            for i in range(10):
                self.comb += self.output[i].eq(output_msb_first[9-i])
        else:
            self.comb += self.output.eq(output_msb_first)

# Encoder ------------------------------------------------------------------------------------------

class Encoder(LiteXModule):
    def __init__(self, nwords=1, lsb_first=False):
        self.ce = Signal(reset=1)
        self.d  = [Signal(8) for _ in range(nwords)]
        self.k  = [Signal() for _ in range(nwords)]
        self.output    = [Signal(10, reset_less=True) for _ in range(nwords)]
        self.disparity = [Signal() for _ in range(nwords)]

        # # #

        encoders = [SingleEncoder(lsb_first) for _ in range(nwords)]
        self.comb += [encoder.ce.eq(self.ce) for encoder in encoders]
        self.submodules += encoders

        self.sync += If(self.ce, encoders[0].disp_in.eq(encoders[-1].disp_out))
        for e1, e2 in zip(encoders, encoders[1:]):
            self.comb += e2.disp_in.eq(e1.disp_out)

        for d, k, output, disparity, encoder in zip(self.d, self.k, self.output, self.disparity, encoders):
            self.comb += [
                encoder.d.eq(d),
                encoder.k.eq(k)
            ]
            output.reset_less = True
            self.sync += If(self.ce,
                output.eq(encoder.output),
                disparity.eq(encoder.disp_out)
            )

# Decoder ------------------------------------------------------------------------------------------

class Decoder(LiteXModule):
    def __init__(self, lsb_first=False, sync=True):
        self.ce      = Signal(reset=1)
        self.input   = Signal(10)
        self.d       = Signal(8)
        self.k       = Signal()
        self.invalid = Signal()

        # # #

        # Destination Domain.
        self.dom = self.sync if sync else self.comb

        input_msb_first = Signal(10)
        if lsb_first:
            for i in range(10):
                self.comb += input_msb_first[i].eq(self.input[9-i])
        else:
            self.comb += input_msb_first.eq(self.input)

        code6b = input_msb_first[4:]
        code5b = Signal(5)
        code4b = input_msb_first[:4]
        code3b = Signal(3, reset_less=True)

        mem_6b5b  = Memory(5, len(table_6b5b), init=table_6b5b)
        port_6b5b = mem_6b5b.get_port(has_re=True, async_read=not sync)
        self.specials += mem_6b5b, port_6b5b
        self.comb += port_6b5b.adr.eq(code6b)
        self.comb += port_6b5b.re.eq(self.ce)

        self.dom += If(self.ce,
            self.k.eq(0),
            If(code6b == 0b001111,
                self.k.eq(1),
                code3b.eq(Array(table_4b3b_kn)[code4b])
            ).Elif(code6b == 0b110000,
                self.k.eq(1),
                code3b.eq(Array(table_4b3b_kp)[code4b])
            ).Else(
                If((code4b == 0b0111) | (code4b == 0b1000),  # D.x.A7/K.x.7
                    If((code6b != 0b100011) &
                       (code6b != 0b010011) &
                       (code6b != 0b001011) &
                       (code6b != 0b110100) &
                       (code6b != 0b101100) &
                       (code6b != 0b011100), self.k.eq(1))
                ),
                code3b.eq(Array(table_4b3b)[code4b])
            ),
        )
        self.comb += code5b.eq(port_6b5b.dat_r)
        self.comb += self.d.eq(Cat(code5b, code3b))

        # Basic invalid symbols detection: check that we have 4,5 or 6 ones in the symbol. This does
        # not report all invalid symbols but still allow detecting issues with the link.
        ones = Signal(4, reset_less=True)
        self.dom  += If(self.ce, ones.eq(Reduce("ADD", [self.input[i] for i in range(10)])))
        self.comb += self.invalid.eq((ones != 4) & (ones != 5) & (ones != 6))


# Stream Encoder -----------------------------------------------------------------------------------

class StreamEncoder(stream.PipelinedActor):
    def __init__(self, nwords=1):
        self.sink   = sink   = stream.Endpoint([("d", nwords*8), ("k", nwords)])
        self.source = source = stream.Endpoint([("data", nwords*10)])
        stream.PipelinedActor.__init__(self, latency=2)

        # # #

        # Encoders
        self.encoder = encoder = Encoder(nwords, True)

        # Control
        self.comb += encoder.ce.eq(self.pipe_ce)

        # Datapath
        for i in range(nwords):
            self.comb += [
                encoder.k[i].eq(sink.k[i]),
                encoder.d[i].eq(sink.d[8*i:8*(i+1)]),
                source.data[10*i:10*(i+1)].eq(encoder.output[i])
            ]

# Stream Encoder -----------------------------------------------------------------------------------

class StreamDecoder(stream.PipelinedActor):
    def __init__(self, nwords=1):
        self.sink   = sink   = stream.Endpoint([("data", nwords*10)])
        self.source = source = stream.Endpoint([("d", nwords*8), ("k", nwords)])
        stream.PipelinedActor.__init__(self, latency=1)

        # # #

        # Decoders
        decoders = [Decoder(True) for _ in range(nwords)]
        self.submodules += decoders

        # Control
        self.comb += [decoders[i].ce.eq(self.pipe_ce) for i in range(nwords)]

        # Datapath
        for i in range(nwords):
            self.comb += [
                decoders[i].input.eq(sink.data[10*i:10*(i+1)]),
                source.k[i].eq(decoders[i].k),
                source.d[8*i:8*(i+1)].eq(decoders[i].d)
            ]