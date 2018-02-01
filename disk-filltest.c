/******************************************************************************
 * disk-filltest.c - Program to fill a hard disk with random data
 *
 * Usage: ./disk-filltest
 *
 * The program will fill the current directory with files called random-#####.
 * Each file is up to 1 GiB in size and contains randomly generated integers.
 * When the disk is full, writing is finished and all files are read from disk.
 * During reading the file contents is checked against the pseudo-random
 * sequence to detect changed data blocks. Any unexpected integer will output
 * an error. Reading and writing speed are shown during operation.
 *
 ******************************************************************************
 * Copyright (C) 2012-2013 Timo Bingmann <tb@panthema.net>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 *****************************************************************************
 * Version 0.8.0W 20171129 https://github.com/Maaciej/disk-filltest
 *****************************************************************************/


#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <Windows.h>
#include <math.h>

/* random seed used */
unsigned int g_seed = 1434038592;

/* only perform read operation */
int gopt_readonly = 0;

/* immediately unlink files after open */
int gopt_unlink_immediate = 0;

/* unlink files after complete test run */
int gopt_unlink_after = 0;

/* individual file size in MiB */
unsigned int gopt_file_size = 1024;

/* file number limit */
unsigned int gopt_file_limit = UINT_MAX;

/* fullfilling params */
unsigned int gopt_sector_size_in512 = 8;
unsigned int fulfill = 0;

/* output conf */
unsigned int multicolor = 0;
unsigned int errors_found = 0;
unsigned int filenumbersize = 0;

/* globals for writing and reading */
double gtimeread=0, gtimewrite=0, gbyteread=0, gbytewrite=0;     // total counts
double gtimereadn=0, gtimewriten=0, gbytereadn=0, gbytewriten=0; // netto without small filling data, for speed calculations

/* return the current timestamp */
static inline double timestamp(void)
{
    struct timeval tv;
    gettimeofday(&tv, 0);
    return ((double)(tv.tv_sec) + (double)(tv.tv_usec/1e6));
}

/* simple linear congruential random generator, faster than rand() and totally
 * sufficient for this cause. */
static inline uint64_t lcg_random(uint64_t *xn)
{
    *xn = 0x27BB2EE687B0B0FDLLU * *xn + 0xB504F32DLU;
    return *xn;
}

/* item type used in blocks written to disk */
typedef uint64_t item_type;

/* a list of open file handles */
int* g_filehandle = NULL;
unsigned int g_filehandle_size = 0;
unsigned int g_filehandle_limit = 0;

//without leading spaces
//https://stackoverflow.com/questions/1449805/how-to-format-a-number-from-1123456789-to-1-123-456-789-in-c

const char *formatNumbernospac (
    uint64_t value,
    char *endOfbuffer
    )
{
    int charCount;

//    if ( value < 0 ) value = - value;

    *--endOfbuffer = 0;
    charCount = -1;

    do
    {
        if ( ++charCount == 3 )
        {
            charCount = 0;
            *--endOfbuffer = ' ';
        }

        *--endOfbuffer = (char) (value % 10 + '0');
    }
    while ((value /= 10) != 0);

    return endOfbuffer;
}

//separated numbers with leading spaces


const char *formatNumber (
    int64_t value,
    char *endOfbuffer
    ,int len
    )
{
    unsigned int i;

    strcpy(endOfbuffer, formatNumbernospac ( value, endOfbuffer));

    len = len - strlen(endOfbuffer);

    if ( len > 0 )
    {
        for (i = 0; i < len ; ++i)
        {
             *--endOfbuffer = ' ';
        }
    }

    return endOfbuffer;
}


/* append to the list of open file handles */
static inline void filehandle_append(int fd)
{
    if (g_filehandle_size >= g_filehandle_limit)
    {
        g_filehandle_limit *= 2;
        if (g_filehandle_limit < 128) g_filehandle_limit = 128;

        g_filehandle = realloc(g_filehandle, sizeof(int) * g_filehandle_limit);
    }

    g_filehandle[ g_filehandle_size++ ] = fd;
}

/* for compatibility with windows, use O_BINARY if available */
#ifndef O_BINARY
#define O_BINARY 0
#endif

/* change console color */
void consoleColor ( char color[20] )
{
/*  USED COLORS

    brightwhite
    cyan
    green
    red
    white
    yellow

    idea from
    https://stackoverflow.com/questions/13280895/how-can-i-use-colors-in-my-console-app-c
*/
    int colorvalue;

    switch ( *color ) {
        case 'b':
            colorvalue = FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_RED | FOREGROUND_INTENSITY;
            break;
        case 'c':
            colorvalue = FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
            break;
        case 'g':
            colorvalue = FOREGROUND_GREEN | FOREGROUND_INTENSITY;
            break;
        case 'r':
            colorvalue = FOREGROUND_RED | FOREGROUND_INTENSITY;
            break;
        case 'w':
            colorvalue = FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_RED;
            break;
        case 'y':
            colorvalue = FOREGROUND_GREEN | FOREGROUND_RED | FOREGROUND_INTENSITY;
            break;
        }

    if ( multicolor == 1 ) SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), colorvalue );

}

/* print command line usage */
void print_usage(char* argv[])
{

    fprintf(stderr,
            "Usage: %s  [-v]  [-C dir] [-g | -s seed] [-S file_size] \n"
            "                          [-f files] [-z | -d block_size] [-u] [-U] [-m]\n"
            "Version 0.8.0W\n"
            "Options: \n"
            "  -v                Verify existing data files.\n"
            "  -C <dir>          Change into given directory before starting work.\n"
            "  -g                Generate random seed.\n"
            "  -s <random seed>  Use this random seed (default=1434038592).\n"
            "  -S <file size>    Size of each file in MiB (default=1024).\n"
            "  -f <file number>  Only write this number of files.\n"
            "  -z                Fill disk with smaller blocks. Other way program fills\n"
            "                           in 1 MiB blocks.     Mutually exclusive with -f. \n"
            "  -d <block size>   Smaller block in 512 B: 4096 B = (block size=8) * 512,\n"
            "                           default=8 (4 KiB). Mutually exclusive with -f.\n"
            "  -u                Remove files after _successful_ test (works with -v).\n"
            "  -U                Immediately remove files, write and verify via file handles\n"
            "                           (not for Windows).\n"
            "  -m                Multicolor detailed output, for dark background.\n"
            "\n"
            "The program will fill the current directory with files called random-XXXXXXXX.\n"
            "Each file is up to 1 GiB (modified with -S) in size and contains randomly\n"
            "generated integers. When there is less then 1 MiB space left (modified \n"
            "with -z; with -d set your cluster size) writing finishes and files are read.\n"
            "Read file contents are checked: every change will output an error. \n"
            "Reading and writing speeds are shown.\n"
            ,argv[0]);

    exit(EXIT_FAILURE);
}

/* parse command line parameters */
void parse_commandline(int argc, char* argv[])
{
    int opt;
    char separated_number[50];

    while ((opt = getopt(argc, argv, "vC:gs:S:f:zd:uUmh")) != -1) {
        switch (opt) {
        case 's':
            g_seed = atoi(optarg);
            break;
         case 'g':
            g_seed = time(NULL);
            break;
        case 'S':
            gopt_file_size = atoi(optarg) ;
            break;
        case 'z':  //zero space left
            fulfill = 1 ;
            break;
        case 'd':
            gopt_sector_size_in512 = atoi(optarg) ;
            fulfill = 1 ;
            break;
        case 'm':
            multicolor = 1 ;
            break;
        case 'f':
            gopt_file_limit = atoi(optarg);
            break;
        case 'v':
            gopt_readonly = 1;
            break;
        case 'u':
            gopt_unlink_after = 1;
            break;
        case 'U':
            gopt_unlink_immediate = 1;
            break;
        case 'C':
            if (chdir(optarg) != 0) {
                printf("Error chdir to %s: %s\n", optarg, strerror(errno));
            }
            break;
        case 'h':
        default:
            print_usage(argv);
        }
    }

    if ( gopt_file_limit != UINT_MAX ) fulfill = 0; //other way, after set number of big files, filling up big disk with small block could take ages, make too much stress and cause other problems

    if (optind < argc)
        print_usage(argv);

    //for formating position numbers
    filenumbersize = strlen( formatNumbernospac ( (uint64_t) gopt_file_size * 1024 * 1024 , separated_number + 22) );
}

/* unlink (delete) old random files */
void unlink_randfiles(void)
{
    unsigned int filenum = 0;
    char filename[32];

    consoleColor("red");

    while (filenum < UINT_MAX)
    {
        char filename[32];
        snprintf(filename, sizeof(filename), "random-%08u", filenum);

        if (unlink(filename) != 0)
            break;

        if (filenum == 0)
            printf("Removing old files .");
        else
            printf(".");
        fflush(stdout);

        ++filenum;
    }

    snprintf(filename, sizeof(filename), "random-%08u", filenum);
    if (unlink(filename) == 0)
            ++filenum;

    if (filenum > 0)
        printf(" total: %u.\n", filenum);

    consoleColor("white");
}

/* fill disk */
void fill_randfiles(void)
{
    unsigned int filenum = 0;
    int done = 0;
    char separated_number[50];
    char path[160];

    printf("Writing files random-XXXXXXXX with seed %u", g_seed);

    if (multicolor == 1 )
    {
        printf(" to directory\n");
        getcwd(path, 160);
        consoleColor("cyan");
        printf("%s", path);
        consoleColor("white");
    }

    printf("\n");

//*****************************************************************
//    ORG WRITE
//*****************************************************************

    while (!done && filenum < gopt_file_limit)
    {
        char filename[32];
        int fd;
        double wtotal;
        ssize_t  wb, wp;
        unsigned int i, blocknum;
        double ts1, ts2;
        uint64_t rnd;

        item_type block[1024*1024 / sizeof(item_type)];

        snprintf(filename, sizeof(filename), "random-%08u", filenum);

        fd = open(filename, O_RDWR | O_CREAT | O_TRUNC | O_BINARY, 0600);
        if (fd < 0) {
            printf("STATUS opening next file %s: %s\n",
                   filename, strerror(errno));
            break;
        }

        if (gopt_unlink_immediate) {
            if (unlink(filename) != 0) {
                printf("Error unlinkin opened file %s: %s\n",
                       filename, strerror(errno));
            }
        }

        /* reset random generator for each file */
        rnd = g_seed + (++filenum);

        wtotal = 0;
        ts1 = timestamp();

        for (blocknum = 0; blocknum < gopt_file_size; ++blocknum)
        {
            for (i = 0; i < sizeof(block) / sizeof(item_type); ++i)
                block[i] = lcg_random(&rnd); /*8!!!!  bytes*/

            wp = 0;

            while ( wp != (ssize_t)sizeof(block) && !done )
            {
                wb = write(fd, (char*)block + wp, sizeof(block) - wp);

                if (wb <= 0) {
                    printf("STATUS writing next file %s: %s\n",
                           filename, strerror(errno));
                    done = 1;
                    break;
                }
                else {
                    wp += wb;
                }
           }

            wtotal += wp;

            if (done) {break;}
        }

        if (gopt_unlink_immediate) { /* do not close file handle! */
            filehandle_append(fd);
             }
        else { close(fd); }

        ts2 = timestamp();
        if ( wtotal == 0 )
        {
            if ( multicolor == 1 ) printf("No space for new file ( 1 MiB block ).\n");
            unlink(filename);
        }
        else
        {
            printf("Wrote %s MB data to %s",formatNumber (wtotal / 1000.0 / 1000.0, separated_number + 20,11),  filename);
            if ( ts2-ts1 != 0 ) printf(" with        % 12.3f MB/s\n"
                                         , wtotal / 1000.0 / 1000.0 / (ts2-ts1) );
            else                printf(" (measured time too short)\n");
        }

        fflush(stdout);

        gbytewrite += wtotal;  gbytewriten = gbytewrite;
        gtimewrite += ts2-ts1; gtimewriten = gtimewrite;
    }

    done = 0;

//*****************************************************************
//    NEW small WRITE
//*****************************************************************

    if ( fulfill == 1 )
    {
        consoleColor("brightWhite");
        printf("Filling up disk with block = %s B", formatNumber (gopt_sector_size_in512* 512,separated_number + 20,7));
        if (multicolor == 1) printf(" (not included in total speed stats)");
        printf("\n");
        consoleColor("white");
    }

    while ( !done && fulfill == 1 )  // filling up
    {

        char filename[32];
        int fd;
        double wtotal;
        ssize_t  wb, wp;
        unsigned int i, blocknum;
        double ts1, ts2;
        uint64_t rnd;

        item_type block2[ gopt_sector_size_in512 * 512 / sizeof(item_type)];  // slow writing
        /* cluster size
        16 KiB on USB 1 GB drive
        4 KiB on half of 256 GB drive partition
        4 KiB on 4 TB drive

        Smaller block, lower speed
        512 B will fill everything, but can be slow
        */

        snprintf(filename, sizeof(filename), "random-%08u", filenum);

        fd = open(filename, O_RDWR | O_CREAT | O_TRUNC | O_BINARY, 0600);
        if (fd < 0) {
            printf("Error opening next file %s: %s\n",
                   filename, strerror(errno));
            break;
        }

        if (gopt_unlink_immediate) {
            if (unlink(filename) != 0) {
                printf("Error unlinking opened file %s: %s\n",
                       filename, strerror(errno));
            }
        }

        /* reset random generator for each file */
        rnd = g_seed + (++filenum);

        wtotal = 0;
        ts1 = timestamp();

        for (blocknum = 0; blocknum < 2048 + 2 ; ++blocknum)
        {
            for (i = 0; i < sizeof(block2) / sizeof(item_type); ++i)
                block2[i] = lcg_random(&rnd); /*  8!!!!  bytes*/

            wp = 0;

            while ( wp != (ssize_t)sizeof(block2) && !done )
            {
                wb = write(fd, (char*)block2 + wp, sizeof(block2) - wp);

                if (wb <= 0) {
                    printf("STATUS writing next file %s: %s\n", filename, strerror(errno));
                    done = 1;
                    break;
                }
                else {
                    wp += wb;
                }
            }

            wtotal += wp;

            if (done) {break;}
        }

        if (gopt_unlink_immediate) { /* do not close file handle! */
            filehandle_append(fd);
             }
        else { close(fd); }

        ts2 = timestamp();

        if ( wtotal == 0 )
        {
            if ( multicolor == 1 ) printf("No space for new file ( %u B block ).\n",gopt_sector_size_in512 * 512 );
            unlink(filename);
        }
        else
        {
            printf("Wrote   % 9.3f kB data to %s ", wtotal / 1000.0,filename);
            if ( ts2-ts1 != 0 )  printf("with        % 12.3f MB/s\n"
                                        , wtotal / 1000.0 / 1000.0 / (ts2-ts1) );
            else                 printf("(measured time too short)\n");
        }

        fflush(stdout);

        gbytewrite += wtotal; gtimewrite += ts2-ts1;
    } // end of small write

    errno = 0;
}

/* read files and check random sequence*/
void read_randfiles(void)
{
    unsigned int filenum = 0;
    int done = 0;
    char path[160];

    printf("Verifying files random-XXXXXXXX with seed %u", g_seed);

    if ( multicolor == 1 && gopt_readonly == 1 )
    {
        printf(" from directory\n");
        getcwd(path, 160);
        consoleColor("cyan");
        printf("%s", path);
        consoleColor("white");
    }

    printf("\n");

//*****************************************************************
//    ORG READ
//*****************************************************************

    while (!done)
    {
        char filename[32];
        int fd;
        double rtotal;
        ssize_t rb;
        unsigned int i, blocknum;
        double ts1, ts2;
        uint64_t rnd;

        char separated_number[50];

        item_type block[1024*1024 / sizeof(item_type)];

        snprintf(filename, sizeof(filename), "random-%08u", filenum);

        /* reset random generator for each file */
        rnd = g_seed + (++filenum);

        if (gopt_unlink_immediate)
        {
            if (filenum >= g_filehandle_size) {
                printf("Finished all opened file handles.\n");
                break;
            }

            fd = g_filehandle[filenum];

            if (lseek(fd, 0, SEEK_SET) != 0) {
                printf("Error seeking in next file %s: %s\n",
                       filename, strerror(errno));
                break;
            }
        }
        else
        {
            fd = open(filename, O_RDONLY | O_BINARY);
            if (fd < 0) {
                printf("Error opening next file %s: %s\n",
                       filename, strerror(errno));
                break;
            }
        }

        rtotal = 0;
        ts1 = timestamp();

        for (blocknum = 0; blocknum < gopt_file_size; ++blocknum)
        {
            rb = read(fd, block, sizeof(block));

            if (rb <= 0) {
                printf("STATUS reading file %s: %s\n",
                       filename, strerror(errno));
                done = 1;
                break;
            }

            rtotal += rb;

            for (i = 0; i < rb  / sizeof(item_type); ++i)
            {
                if (block[i] != lcg_random(&rnd))
                {
                    ++errors_found;
                    consoleColor("red");
                    printf("ERROR! %s Position: %s BLOCK:% 6lu OFFSET:% 7lu\n", filename
                            , formatNumber ( (uint64_t) blocknum * 1024 * 1024+ (uint64_t) (i * sizeof(item_type)), separated_number + 20,filenumbersize+1)
                           ,blocknum, (uint64_t) (i * sizeof(item_type)));

                    consoleColor("white");
                    gopt_unlink_after = 0;
//                    break; //with this break 1. other errors in this block are not reported, and
//                                             2. error is in every other block because lcg_random is not executed for every integer
                }
            }

            if (done) {break;}

        }

        close(fd);

        ts2 = timestamp();

        printf("Read     %s MB data from %s",
               formatNumber (rtotal / 1000.0 / 1000.0, separated_number + 20,8), filename);
        if ( ts2-ts1 != 0 ) printf(" with      % 12.3f MB/s \n"
                           ,(rtotal / 1000 / 1000 / (ts2-ts1)));
        else // bad values for MB/s if very short time, divide by zero
                            printf(" (measured time too short)\n");

        fflush(stdout);

         gbyteread += rtotal;
         gtimeread += ts2-ts1;

    }

    gbytereadn = gbyteread;gtimereadn = gtimeread;

    done=0;

//*****************************************************************
//    NEW small READ
//*****************************************************************

        while ( !done && gopt_file_limit == UINT_MAX && fulfill == 1  )
    {  // testing "small" write
        char filename[32];
        int fd;
        double rtotal;
        ssize_t rb;
        unsigned int i, blocknum;
        double ts1, ts2;
        uint64_t rnd;

        char separated_number[50];

        item_type block[ gopt_sector_size_in512 * 512 / sizeof(item_type)];

        snprintf(filename, sizeof(filename), "random-%08u", filenum);

        if (gopt_unlink_immediate)
        {
            if (filenum >= g_filehandle_size)
            {
                printf("Finished all opened file handles.\n");
                break;
            }

            fd = g_filehandle[filenum];

            if (lseek(fd, 0, SEEK_SET) != 0)
            {
                printf("Error seeking in next file %s: %s\n",
                        filename,strerror(errno));
                break;
            }
        }
        else
        {
            fd = open(filename, O_RDONLY | O_BINARY);
            if (fd < 0) {
                printf("Error opening next file %s: %s\n",
                        filename,strerror(errno));
                break;
            }
        }

        /* reset random generator for each file */
        rnd = g_seed + (++filenum);

        rtotal = 0;
        ts1 = timestamp();

        for (blocknum = 0; blocknum < 2048 + 2; ++blocknum)  // 2048 = 1024 * 1024 / 512 = max number (?) of 512 B sectors for 1 MiB block
        {
            rb = read(fd, block, sizeof(block));

            if (rb <= 0) {
                printf("STATUS reading file %s: %s\n",
                        filename,strerror(errno));
                done = 1;
                break;
                 }

            for (i = 0; i < rb  / sizeof(item_type); ++i)
            {

                if (block[i] != lcg_random(&rnd))
                {
                    ++errors_found;

                    consoleColor("red");

                    printf("ERROR! %s Position: %s BLOCK:% 6lu OFFSET:% 7lu\n", filename
                            , formatNumber ((uint64_t)blocknum * (uint64_t)gopt_sector_size_in512 * 512+ (uint64_t) (i * sizeof(item_type)), separated_number + 20,filenumbersize+1)
                           ,blocknum,  (uint64_t) (i * sizeof(item_type)));

                    consoleColor("white");
                    gopt_unlink_after = 0;
//                    break;
                }
            }

            rtotal += rb;

        }

        close(fd);

        ts2 = timestamp();

        printf("Read    % 9.3f kB data from %s ",
               (rtotal / 1000),               filename);

        if ( ts2-ts1 != 0 ) printf("with      % 12.3f MB/s\n"
                                   , rtotal / 1000 / 1000 / (ts2-ts1) );
        else
                            printf(" (measured time too short)\n");

        fflush(stdout);

        gbyteread += rtotal; gtimeread += ts2-ts1;

    }
}

//
// MAIN
//

int main(int argc, char* argv[])
{
    double gts, gte; //global start and end
    time_t curtime;
    struct tm * curtimestruct;
    char separated_number[50];

    parse_commandline(argc, argv);

    gts = timestamp();

    if (gopt_readonly == 0)
    {
        unlink_randfiles();

            if (multicolor == 1)
            {
                consoleColor("green");
                curtime = time(NULL); curtimestruct = localtime(&curtime);printf("START WRITING  %s", asctime(curtimestruct));
                consoleColor("white");
            };

        fill_randfiles();

        if (multicolor == 1)
        { //write stat
            consoleColor("yellow");
            curtime = time(NULL); curtimestruct = localtime(&curtime);printf("END   WRITING  %s", asctime(curtimestruct));

            printf("Wrote %s MB in % 4.0f h %02.0f m %02.0f s %03.0f ms", formatNumber (gbytewrite / 1000.0 / 1000.0, separated_number + 20,11),
                     floor((gtimewrite)/3600), floor( ( (gtimewrite) - floor((gtimewrite)/3600)*3600  )/60),floor((gtimewrite) - floor((gtimewrite)/60)*60 ), 1000*((gtimewrite) - floor(gtimewrite) ));
            if (gtimewriten != 0 )    printf("          % 12.3f MB/s\n"
                                                ,gbytewriten / 1000 / 1000 / (gtimewriten));
            else                      printf(" (measured time too short)\n");
        };

    }
    if (multicolor == 1)
    {
        consoleColor("green");
        curtime = time(NULL); curtimestruct = localtime(&curtime);printf("START READING  %s", asctime(curtimestruct));
        consoleColor("white");
    }


    read_randfiles();

    if (multicolor == 1)
    {
        consoleColor("yellow");
        curtime = time(NULL); curtimestruct = localtime(&curtime);printf("END   READING  %s", asctime(curtimestruct));
        consoleColor("white");
    }

    if ( gopt_readonly == 1 && gopt_unlink_after )
            unlink_randfiles();

    gte = timestamp();


    if ( gopt_readonly == 0 )
      {
        if ( multicolor == 1 && gbytewrite != 0 )
        {// total write statistics

            printf("Wrote %s MB in % 4.0f h %02.0f m %02.0f s %03.0f ms",formatNumber (gbytewrite / 1000.0 / 1000.0, separated_number + 20,11)
                   ,floor((gtimewrite)/3600), floor( ( (gtimewrite) - floor((gtimewrite)/3600)*3600  )/60),floor((gtimewrite) - floor((gtimewrite)/60)*60 ),  1000*((gtimewrite) - floor(gtimewrite) ));
            if (gtimewriten !=0 ) printf("          % 12.3f MB/s\n",
                                        gbytewriten / 1000 / 1000 / gtimewriten);
            else                  printf(" (measured time too short)\n");

            fflush(stdout);
        };
      };

    if ( multicolor == 1 && gbyteread != 0 )
    { // total read statistics
        printf("Read  %s MB in % 4.0f h %02.0f m %02.0f s %03.0f ms",
                    formatNumber (gbyteread / 1000.0 / 1000.0, separated_number + 20,11)
                    ,floor((gtimeread)/3600), floor( ( (gtimeread) - floor((gtimeread)/3600)*3600  )/60),floor((gtimeread) - floor((gtimeread)/60)*60 ), 1000*((gtimeread) - floor(gtimeread) ));
        if (gtimereadn != 0)  printf("          % 12.3f MB/s\n"
                                        ,gbytereadn / 1000 / 1000 / gtimereadn);
        else
                              printf(" (measured time too short)\n");
        fflush(stdout);
    };

   if (multicolor == 1)
    { // total test time
        consoleColor("yellow");
        printf("TEST TIME  =            % 4.0f h %02.0f m %02.0f s %03.0f ms \n",floor((gte-gts)/3600), floor( ( (gte-gts) - floor((gte-gts)/3600)*3600  )/60),floor((gte-gts) - floor((gte-gts)/60)*60 ), 1000*((gte-gts) - floor(gte-gts) )  );
    };


    if (errors_found != 0)
    {
        consoleColor("red");
        printf(" %u ERRORS found!!!!\n", errors_found);
    }
    else
    {

        consoleColor("green");
        printf("NO errors found.\n");
    }


    if ( ( fulfill == 1 || g_seed != 1434038592 || gopt_file_size != 1024 ) && gopt_readonly == 0 && gopt_unlink_immediate == 0 && gbytewrite >0 )
    { // test tip
        consoleColor("cyan");
        printf("Use this parameters to test created files later: \n -v ");

        if ( gopt_file_size != 1024  ) printf("-S %u", gopt_file_size);
        if ( g_seed != 1434038592  ) printf(" -s %u", g_seed);

        if ( gopt_sector_size_in512 != 8 ) printf(" -d %u", gopt_sector_size_in512);
        else
        if ( fulfill == 1 ) printf(" -z");

        printf("\n");

    }

    consoleColor("white");

    return 0;
}
