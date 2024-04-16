

def gen_table(wide_max, thin_max):
  print('[')
  num_on_line = 0
  for wide_val in range(0, wide_max):
    thin_val = int((float(wide_val) / float(wide_max)) * float(thin_max) )
    #print(f'  {wide_val}: {thin_val},')
    print(f'{thin_val},', end='', flush=True) # wide_val becomes index
    num_on_line += 1
    if num_on_line > 16:
      print('')
      num_on_line = 0
  print(']')

print('8 to 5 bits:')
gen_table(256, 2 ** 5)

print('8 to 6 bits:')
gen_table(256, 2 ** 6)

