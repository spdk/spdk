#!/usr/bin/env perl

use strict;
use warnings;

use constant SEEK_CUR => 1;

( @ARGV == 2 ) || exit(1);

my $cpu_path = sprintf( "/dev/cpu/%u/msr", shift() );
my $msr      = hex( shift() );
my $reg_size = 8;
my ( @msr, $msr_buf, $reg );

unless ( -e $cpu_path ) {
    printf STDERR "$cpu_path doesn't exist\n";
    exit(1);
}

open( MSR, "<", $cpu_path );
sysseek( MSR, $msr, SEEK_CUR );
sysread( MSR, $msr_buf, $reg_size );
@msr = unpack( "C*", $msr_buf );

unless ( @msr == $reg_size ) {
    printf STDERR "Failed to read $cpu_path\n";
    exit(1);
}

for ( my $byte = @msr - 1 ; $byte >= 0 ; $byte-- ) {
    $reg |= $msr[$byte] << ( $byte * 8 );
}

printf( "0x%x\n", $reg );
