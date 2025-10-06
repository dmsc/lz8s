/*
 * LZ8S ultra-simple LZ based decompressor
 * ---------------------------------------
 *
 * (c) 2025 DMSC
 * Code under MIT license, see LICENSE file.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
void set_binary(void)
{
  setmode(fileno(stdout),O_BINARY);
  setmode(fileno(stdin),O_BINARY);
}
#else
void set_binary(void)
{
}
#endif

static int bits_moff = 8;       // Number of bits used for OFFSET
static int max_mlen = 255;      // Maximum match length (unlimited in LZ4)
static int max_llen = 255;      // Maximum literal length (unlimited in LZ4)
static int zero_offset = 0;     // Do not read offset on matches of length 0
static int offset_rel = -1;     // Offset relative or absolute
static int exor_offset = 0;     // Write inverse of offset

// Read match/literal length - depends on max length
static int get_len(int max)
{
    int c = getchar();
    if( c == EOF )
        return -1;
    if(max < 256 || c < 128)
        return c;
    int c2 = getchar();
    if( c == EOF )
    {
        fprintf(stderr, "ERROR, end of file reading second byte of length");
        return -1;
    }
    return c + (c2 << 7);
}

// Decoding function - this is extremely simple (by design!)
int decode(void)
{
    // Maximum window: 16bit
    char buf[65536];
    unsigned pos = 0;
    unsigned mask = bits_moff > 8 ? 0xFFFF : 0xFF;
    int n, x;

    while(1)
    {
        // Decode LITERAL
        if( (n = get_len(max_llen)) < 0 )
            return pos;

        // Copy from input (LITERAL)
        for(int i = 0; i < n; i++)
        {
            if (EOF == (x = getchar()))
            {
                fprintf(stderr, "ERROR, short file reading literal.\n");
                return pos;
            }
            buf[pos & mask] = x;
            putchar(x);
            pos++;
        }

        // Decode MATCH
        if( (n = get_len(max_mlen)) < 0 )
            return pos;

        if( zero_offset || n )
        {
            // Read match offset
            int off = 0;
            if(bits_moff > 0)
            {
                if (EOF == (off = getchar()))
                {
                    fprintf(stderr, "ERROR, short file reading match offset.\n");
                    return pos;
                }
            }
            if(bits_moff > 8)
            {
                if (EOF == (x = getchar()))
                {
                    fprintf(stderr, "ERROR, short file reading match offset.\n");
                    return pos;
                }
                off = off + (x << 8);
            }
            if( exor_offset )
                off = mask ^ off;
            if( offset_rel < 0 )
                off = pos - off + mask;
            else
                off = off + mask + 1 - offset_rel;

            // Copy from old output (MATCH)
            for(int i = 0; i < n; i++)
            {
                x = buf[off & mask];
                buf[pos & mask] = x;
                putchar(x);
                pos++;
                off++;
            }
        }
    }
    return 0;
}

static const char *prog_name;
static void cmd_error(const char *msg)
{
    fprintf(stderr,"%s: error, %s\n"
            "Try '%s -h' for help.\n", prog_name, msg, prog_name);
    exit(1);
}

int main(int argc, char **argv)
{
    int verbose = 0;

    prog_name = argv[0];
    int opt;
    while( -1 != (opt = getopt(argc, argv, "hvnxo:l:m:A:")) )
    {
        switch(opt)
        {
            case 'o':
                bits_moff = atoi(optarg);
                break;
            case 'l':
                max_llen = atoi(optarg);
                break;
            case 'm':
                max_mlen = atoi(optarg);
                break;
            case 'A':
                offset_rel = strtol(optarg, 0, 0);
                break;
            case 'x':
                exor_offset = 1;
                break;
            case 'n':
                zero_offset = 1;
                break;
            case 'v':
                verbose = 1;
                break;
            case 'h':
            default:
                fprintf(stderr,
                       "LZ8D ultra-simple LZ based decompressor - by dmsc.\n"
                       "\n"
                       "Usage: %s [options] <input_file> <output_file>\n"
                       "\n"
                       "If output_file is omitted, write to standard output, and if\n"
                       "input_file is also omitted, read from standard input.\n"
                       "\n"
                       "Options:\n"
                       "  -o BITS  Sets match offset bits (default = %d).\n"
                       "  -l NUM   Sets max literal run length (default = %d).\n"
                       "  -m NUM   Sets max match run length (default = %d).\n"
                       "  -A ADDR  Decode position relative to address instead of offset.\n"
                       "  -n       Do not omit match offset on zero match length.\n"
                       "  -x       Offsets are inverted.\n"
                       "  -v       Shows compression statistics.\n"
                       "  -h       Shows this help.\n",
                       prog_name, bits_moff, max_llen, max_mlen);
                exit(EXIT_FAILURE);
        }
    }

    // Check option values
    if( max_mlen < 1 || max_mlen > 32895 )
        cmd_error("max match run length should be from 1 to 32895");
    if( max_llen < 1 || max_llen > 32895 )
        cmd_error("max literal run length should be from 1 to 32895");
    if( bits_moff < 0 || bits_moff > 16 )
        cmd_error("match offset bits should be from 0 to 16");
    if(bits_moff == 8)
    {
        if(offset_rel > 0xFF)
            cmd_error("relative address should be less than 256 with 8 bit offsets");
    }
    else if(bits_moff == 16)
    {
        if( offset_rel > 0xFFFF )
            cmd_error("relative address should be less than 65536");
    }
    else if(offset_rel >= 0)
        cmd_error("relative address works only with 8 or 16 bit offsets");

    if( optind < argc-2 )
        cmd_error("too many arguments: one input file and one output file expected");

    FILE *input_file = stdin;
    if( optind < argc )
    {
        input_file = fopen(argv[optind], "rb");
        if( !input_file )
        {
            fprintf(stderr, "%s: can't open input file '%s': %s\n",
                    prog_name, argv[optind], strerror(errno));
            exit(EXIT_FAILURE);
        }
    }
    // Set stdin and stdout as binary files
    set_binary();

    // Now, main decoding - this is extremely simple (by design!)
    int size = decode();

    if(verbose)
        fprintf(stderr, "Output size: %d\n", size);

    return 0;
}

