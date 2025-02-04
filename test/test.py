import random
import os 

FTL_INSTANCES = 4
PAGE_SIZE = 32768

FILENAME = '/dev/nvme2n1'
IO_ENGINE = 'libaio'
NUMJOBS = 4
IO_DEPTH = 16
SIZE = '4G'
BS = '128K'

def size_parser(size):
    if size[-1] == 'K' or size[-1] == 'k':
        return int(size[:-1]) * 1024
    elif size[-1] == 'M' or size[-1] == 'm':
        return int(size[:-1]) * 1024 * 1024
    elif size[-1] == 'G' or size[-1] == 'g':
        return int(size[:-1]) * 1024 * 1024 * 1024
    else:
        return int(size)

def linear(cnt):
    return [i for i in range(cnt * FTL_INSTANCES)]

def random_access(cnt):
    testset = [i for i in range(cnt * FTL_INSTANCES)]
    random.shuffle(testset)
    return testset

def round_robin(cnt):
    result = []
    testset = [[] for _ in range(FTL_INSTANCES)]
    bpp = PAGE_SIZE // size_parser(BS)
    gap = bpp * FTL_INSTANCES
    
    for j in range(FTL_INSTANCES):
        for i in range(cnt):
            testset[j].append(i % bpp + gap * (i // bpp) + j * bpp)
        
        random.shuffle(testset[j])
    
    for i in range(cnt):
        for j in range(FTL_INSTANCES):
            result.append(testset[j][i])
    
    return result

def round_robin_per_pages(cnt):
    result = []
    testset = [[] for _ in range(FTL_INSTANCES)]
    bpp = PAGE_SIZE // size_parser(BS)
    gap = bpp * FTL_INSTANCES
    page_num = [i for i in range(cnt // bpp)]
    
    return page_num

def save_testset(testset):
    filename = os.path.join(os.path.dirname(__file__), 'testset.txt')
    with open(filename, 'w') as f:
        for t in testset:
            f.write(f'{t}\n')

def run_test(testtype, bs=BS):
    cnt = size_parser(SIZE) // size_parser(bs) // FTL_INSTANCES
    testset = testtype(cnt)
    save_testset(testset)
    
    print('Compile test.c')
    test_filename = os.path.join(os.path.dirname(__file__), 'test')
    test_c_filename = os.path.join(os.path.dirname(__file__), 'test.c')
    os.system(f'gcc -Wall -O2 -I/usr/include -o {test_filename} {test_c_filename} -L/usr/lib/x86_64-linux-gnu -laio -luring -lpthread')
    
    # run test
    print('Run test')
    print(f'Arguments: filename={FILENAME}, io_engine={IO_ENGINE}, numjobs={NUMJOBS}, io_depth={IO_DEPTH}, size={SIZE}, bs={BS}')
    os.system(f'sudo {test_filename} -f {FILENAME} -m {IO_ENGINE} -j {NUMJOBS} -q {IO_DEPTH} -t {SIZE} -b {bs}')

if __name__ == '__main__':
    run_test(linear, bs='4K')
    run_test(linear, bs='8K')
    run_test(linear, bs='16K')
    run_test(linear, bs='32K')
    run_test(linear, bs='64K')
    run_test(linear, bs='128K') 