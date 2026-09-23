use blip25_vocoder::halfrate::frame::{encode_code_vectors, CODE_WIDTHS};
use blip25_vocoder::vocoder::{Rate, Vocoder, FRAME_SAMPLES};

pub const AMBE_BYTES: usize = 9;

pub struct Encoder {
    vocoder: Vocoder,
}

impl Encoder {
    pub fn new() -> Self {
        Self {
            vocoder: Vocoder::new(Rate::HalfRate3600x2450),
        }
    }

    pub fn reset(&mut self) {
        self.vocoder.reset();
    }

    pub fn encode(&mut self, pcm: &[i16; FRAME_SAMPLES]) -> Option<[u8; AMBE_BYTES]> {
        let info = self.vocoder.encode_info(pcm).ok()?;
        let info: [u16; 4] = info.try_into().ok()?;
        let code = encode_code_vectors(&info);
        Some(code_vectors_to_bytes(&code))
    }
}

fn code_vectors_to_bytes(code: &[u32; 4]) -> [u8; AMBE_BYTES] {
    let mut out = [0u8; AMBE_BYTES];
    let mut bit = 0usize;

    for (i, &word) in code.iter().enumerate() {
        for k in (0..CODE_WIDTHS[i] as usize).rev() {
            if ((word >> k) & 1) != 0 {
                out[bit >> 3] |= 1 << (7 - (bit & 7));
            }
            bit += 1;
        }
    }

    debug_assert_eq!(bit, 72);
    out
}

#[repr(C)]
pub struct Iu2vtpAmbeEncoder {
    inner: Encoder,
}

#[no_mangle]
pub extern "C" fn iu2vtp_ambe_encoder_create() -> *mut Iu2vtpAmbeEncoder {
    Box::into_raw(Box::new(Iu2vtpAmbeEncoder {
        inner: Encoder::new(),
    }))
}

#[no_mangle]
pub unsafe extern "C" fn iu2vtp_ambe_encoder_destroy(enc: *mut Iu2vtpAmbeEncoder) {
    if !enc.is_null() {
        drop(Box::from_raw(enc));
    }
}

#[no_mangle]
pub unsafe extern "C" fn iu2vtp_ambe_encoder_reset(enc: *mut Iu2vtpAmbeEncoder) -> bool {
    let Some(enc) = enc.as_mut() else {
        return false;
    };
    enc.inner.reset();
    true
}

#[no_mangle]
pub unsafe extern "C" fn iu2vtp_ambe_encode_pcm160(
    enc: *mut Iu2vtpAmbeEncoder,
    pcm: *const i16,
    ambe9: *mut u8,
) -> bool {
    let Some(enc) = enc.as_mut() else {
        return false;
    };
    if pcm.is_null() || ambe9.is_null() {
        return false;
    }

    let pcm_slice = core::slice::from_raw_parts(pcm, FRAME_SAMPLES);
    let Ok(pcm_frame) = <&[i16; FRAME_SAMPLES]>::try_from(pcm_slice) else {
        return false;
    };

    let Some(encoded) = enc.inner.encode(pcm_frame) else {
        return false;
    };

    core::ptr::copy_nonoverlapping(encoded.as_ptr(), ambe9, AMBE_BYTES);
    true
}

#[cfg(test)]
mod tests {
    use super::*;
    use blip25_vocoder::halfrate::frame::{decode_code_vectors, CODE_WIDTHS};
    use blip25_vocoder::vocoder::{FrameStatus, Vocoder};

    fn bytes_to_code_vectors(bytes: &[u8; 9]) -> [u32; 4] {
        let mut bit = 0usize;
        core::array::from_fn(|i| {
            let mut word = 0u32;
            for _ in 0..CODE_WIDTHS[i] {
                let v = (bytes[bit >> 3] >> (7 - (bit & 7))) & 1;
                word = (word << 1) | u32::from(v);
                bit += 1;
            }
            word
        })
    }

    fn tone() -> [i16; FRAME_SAMPLES] {
        core::array::from_fn(|i| {
            let t = i as f32 / 8000.0;
            (7000.0 * (2.0 * core::f32::consts::PI * 440.0 * t).sin()) as i16
        })
    }

    #[test]
    fn encode_roundtrip_produces_audio() {
        let mut enc = Encoder::new();

        // Encoder has one-frame algorithmic delay. Prime it, then encode again.
        let _ = enc.encode(&tone()).expect("prime");
        let ambe = enc.encode(&tone()).expect("encode");

        let frame = decode_code_vectors(bytes_to_code_vectors(&ambe));
        let mut dec = Vocoder::new(Rate::HalfRate3600x2450);
        let pcm = dec
            .decode_info(&frame.info, FrameStatus::CLEAN)
            .expect("decode");

        assert_eq!(pcm.len(), FRAME_SAMPLES);
        let energy: u64 = pcm.iter().map(|&v| i64::from(v).unsigned_abs()).sum();
        assert!(energy > 0);
    }
}
