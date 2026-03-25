import math

def generate_piano_wavetable():
    size = 256
    table = []
    for i in range(size):
        t = i / size * 2 * math.pi
        # Piano harmonics: Fundamental, strong 2nd/3rd, dropping off
        val = math.sin(t) + 0.6 * math.sin(2*t) + 0.4 * math.sin(3*t) + 0.2 * math.sin(4*t) + 0.1 * math.sin(5*t)
        table.append(val)
        
    # Normalize to -127 to 127
    max_val = max(abs(x) for x in table)
    int_table = [int(round(x / max_val * 127)) for x in table]
    
    # Format as C array
    c_array = "static const int8_t piano_wavetable[256] = {\n    "
    for i, val in enumerate(int_table):
        c_array += f"{val:4},"
        if (i + 1) % 16 == 0 and i != 255:
            c_array += "\n    "
    c_array += "\n};"
    
    with open("wavetable.txt", "w") as f:
        f.write(c_array)
        
if __name__ == "__main__":
    generate_piano_wavetable()
