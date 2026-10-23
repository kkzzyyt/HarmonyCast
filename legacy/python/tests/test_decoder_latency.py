from fractions import Fraction
import re

import av
import numpy as np

from harmonycast.upstream.config import VideoStreamConfig
from harmonycast.upstream.decoder import VideoDecoder


def test_each_complete_picture_is_displayed_without_waiting_for_the_next_picture():
    encoder = av.CodecContext.create("libx264", "w")
    encoder.width, encoder.height = 64, 64
    encoder.pix_fmt = "yuv420p"
    encoder.time_base = Fraction(1, 30)
    encoder.framerate = Fraction(30, 1)
    encoder.options = {"preset": "ultrafast", "tune": "zerolatency"}
    decoder = VideoDecoder(VideoStreamConfig(64, 64, 30, 1_500_000, "h264"))
    for index, channel in enumerate((1, 0, 2)):
        image = np.zeros((64, 64, 3), dtype=np.uint8)
        image[:, :, channel] = 220
        frame = av.VideoFrame.from_ndarray(image, format="rgb24")
        frame.pts = index
        packet = encoder.encode(frame)[0]
        raw = bytes(packet)
        if index == 0:
            nals = [part for part in re.split(b"\x00\x00\x00?\x01", raw) if part]
            decoder.set_sps(next(nal for nal in nals if nal[0] & 31 == 7))
            decoder.set_pps(next(nal for nal in nals if nal[0] & 31 == 8))
        result = decoder.decode_frame(raw, is_keyframe=packet.is_keyframe)
        assert result is not None, "The current picture must be available before any later packet arrives"
        assert result[:, :, channel].mean() > 200
        assert result[:, :, (channel + 1) % 3].mean() < 20
    decoder.cleanup()
