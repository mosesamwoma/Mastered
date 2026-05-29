#!/usr/bin/env python3
"""
Generate test WAV files for mastering engine testing
Creates sine wave test tracks at various frequencies
"""

import struct
import math
import sys
from pathlib import Path

def write_wav(filename, sample_rate, samples):
    """Write samples to a WAV file"""
    num_samples = len(samples)
    num_channels = 1
    bits_per_sample = 16
    byte_rate = sample_rate * num_channels * bits_per_sample // 8
    block_align = num_channels * bits_per_sample // 8
    
    with open(filename, 'wb') as f:
        # RIFF header
        f.write(b'RIFF')
        file_size = 36 + num_samples * block_align
        f.write(struct.pack('<I', file_size))
        f.write(b'WAVE')
        
        # fmt chunk
        f.write(b'fmt ')
        f.write(struct.pack('<I', 16))  # chunk size
        f.write(struct.pack('<HHIIHH', 
                           1,              # audio format (PCM)
                           num_channels,   # channels
                           sample_rate,    # sample rate
                           byte_rate,      # byte rate
                           block_align,    # block align
                           bits_per_sample))  # bits per sample
        
        # data chunk
        f.write(b'data')
        f.write(struct.pack('<I', num_samples * block_align))
        
        # Write samples
        for sample in samples:
            # Clamp to [-1, 1] and convert to 16-bit
            sample = max(-1.0, min(1.0, sample))
            pcm_value = int(sample * 32767)
            f.write(struct.pack('<h', pcm_value))

def generate_tone(frequency, duration, sample_rate, amplitude=0.8):
    """Generate sine wave tone"""
    num_samples = int(duration * sample_rate)
    samples = []
    
    for i in range(num_samples):
        t = i / sample_rate
        sample = amplitude * math.sin(2 * math.pi * frequency * t)
        samples.append(sample)
    
    return samples

def generate_reference_track(duration=5, sample_rate=44100):
    """Generate reference mastering track with multiple frequencies"""
    samples = []
    
    # Composite signal: mix of different frequencies
    # This represents a "well-mastered" track
    frequencies = [60, 250, 1000, 4000, 12000]  # Low, low-mid, mid, high-mid, high
    amplitudes = [0.15, 0.20, 0.25, 0.20, 0.10]  # Different amplitude per band
    
    num_samples = int(duration * sample_rate)
    
    for i in range(num_samples):
        t = i / sample_rate
        sample = 0.0
        
        for freq, amp in zip(frequencies, amplitudes):
            sample += amp * math.sin(2 * math.pi * freq * t)
        
        # Add gentle compression curve (simulate loudness)
        sample = sample * 0.9
        samples.append(sample)
    
    return samples

def generate_unmastered_track(duration=5, sample_rate=44100):
    """Generate unmastered track - too much bass, thin treble"""
    samples = []
    
    # Exaggerated bass, reduced high frequencies
    num_samples = int(duration * sample_rate)
    
    for i in range(num_samples):
        t = i / sample_rate
        sample = 0.0
        
        # Too much bass
        sample += 0.40 * math.sin(2 * math.pi * 60 * t)
        
        # Reduced mid-range
        sample += 0.10 * math.sin(2 * math.pi * 250 * t)
        sample += 0.12 * math.sin(2 * math.pi * 1000 * t)
        
        # Weak high-mid
        sample += 0.08 * math.sin(2 * math.pi * 4000 * t)
        
        # Barely any high-frequency content
        sample += 0.02 * math.sin(2 * math.pi * 12000 * t)
        
        samples.append(sample)
    
    return samples

def main():
    output_dir = Path(__file__).resolve().parent / 'test_audio'
    output_dir.mkdir(parents=True, exist_ok=True)
    
    print("Generating test WAV files...")
    print(f"Output directory: {output_dir}\n")
    
    sample_rate = 44100
    duration = 5
    
    # Generate reference track
    print("Generating reference track (well-mastered)...")
    ref_samples = generate_reference_track(duration, sample_rate)
    ref_path = output_dir / 'reference.wav'
    write_wav(str(ref_path), sample_rate, ref_samples)
    print(f"  ✓ Saved: {ref_path}")
    
    # Generate unmastered track
    print("Generating unmastered track (bass-heavy, thin treble)...")
    unmastered_samples = generate_unmastered_track(duration, sample_rate)
    unmastered_path = output_dir / 'unmastered.wav'
    write_wav(str(unmastered_path), sample_rate, unmastered_samples)
    print(f"  ✓ Saved: {unmastered_path}")
    
    print("\n✓ Test files created successfully!")
    print(f"\nTest with:")
    print(f"  ./build/mastered_cli {ref_path} {unmastered_path} result.json")

if __name__ == '__main__':
    main()
