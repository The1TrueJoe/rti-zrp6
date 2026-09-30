"""Talking to an RTI ZRP-6 whose PLD and firmware were rebuilt from scratch.

`device.Zrp6` is the transport: a small binary protocol over UDP to the board's
own firmware. `rti` decodes RTI remote presses arriving on SIGNAL IN.
"""
from .device import Zrp6, nec_pairs, pronto          # noqa: F401
