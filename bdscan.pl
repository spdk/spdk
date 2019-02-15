#!/usr/bin/env perl
#---------------------------------------------------------------------------+
# 
# Author      : shaw.ming.tan@intel.com
# Description :
# 
# This script bdscan.pl is built on top of ProtexIP to automate the protexip
# scanning to be use in unattended fashion. It is presume the executor are 
# familiar with protexip before using this script. Some additional Perl module 
# might be require dependent on Perl distribution on the host environment.
#
#---------------------------------------------------------------------------+
# History     :
#
# - [31-03-2011] stan55, v1.0
# - [25-04-2011] stan55, v1.0.1
#   > added multiple cfg scanning support. Remove the -srcdir feature.
# - [01-06-2011] stan55, v1.0.2
#   > added urgent flag on email when issues detected
#   > added color to easily identify protexip project having problem
#   > added support of having email address defined on commandline
#   > fixed duplicate email address issue when multiple config were use
#   > quick fix on activestate perl numerious warning. Latest activestate perl 
#     distribution seem to activate warning by default.
# - [03-08-2011] stan55, v1.0.2-fix
#   > remove log file if email sent successfully
# - [23-08-2011] stan55, v1.0.3
#   > added text file format convertion when reading windows-based bomlist in unix environment
#   > minor layout facelift
# - [21-11-2011] stan55, v1.0.4
#   > strip spaces in the email list defined [ESSUP00010314]
# - [29-02-2012] stan55, v1.0.5
#   > added support for post-processing patch files (.patch/.diff) by removing leading "+" character.
# - [06-04-2013] stan55, v1.1
#   > change default scanning mode to incremental and add -force switch for fullscan.
#   > change SMTP server to generic Intel SMTP server.
# - [06-13-2014] stan55, v1.1.1
#   > fixed the issue of incremental scan dishonour server side files pattern matching.
# - [11-21-2014] stan55, v1.2
#   > add support of environment variable expansion on server, username and password in config file.
#
#---------------------------------------------------------------------------+
#
# Copyright (c) 2011-2014 by Engineering Service. All rights reserved.
#
# Redistribution and use in source and binary forms, with or without 
# modification, are permitted provided that any reliance upon the material
# use as their own risk.
# 
#---------------------------------------------------------------------------+

# List of require modules
# which is common on most 5.8 perl distribution
use strict;
use POSIX qw(strftime);
use Getopt::Long;
use File::Basename;
use File::Path;
use File::Find;
use File::Spec;
use File::Copy;
use Cwd;
use Net::SMTP;
use MIME::Base64;
use LWP::MediaTypes;

# Get temp location
use File::Temp qw/ tempfile tempdir /;

# enable autoflush;
local $| = 1;

# Enumerated inputs for printHelp.
use constant MSG_OK => 0;
use constant MSG_WARN => 1; 
use constant MSG_ERR => 2;
use constant MSG_OTHER => 3;

# Message formatting strings
use constant INFO_PREFIX => "INFO : ";
use constant WARNING_PREFIX => "WARN : ";
use constant ERROR_PREFIX => "ERR  : ";

# Command Line Variable
my ($HELP, $PREFIX, @CFG, $LISTID, $EMAIL, $DEBUG, $DRYRUN, $FORCE);

# Set default value
$LISTID = 0;
$DRYRUN = 0;
$DEBUG  = 0;
$FORCE  = 0; # default incremental scanning

my @cmdlineoptions = (
                      "help|h|?" => \$HELP,
                      "prefix:s" => \$PREFIX,
                      "cfg=s" => \@CFG,
                      "force" => \$FORCE,
                      "listid" => \$LISTID,
                      "email:s" => \$EMAIL,
                      "dryrun" => \$DRYRUN,
                      "debug" => \$DEBUG,                      
                      );

# Validating input argument
my $retVal = &ProcessCmdLineOptions(@cmdlineoptions);
unless ( $retVal ) {
    exit -1;
}

# Global variable
my ( @bom, $logfile );
my $timestamp = strftime( "%Y%m%d", localtime );
my $pwd = getcwd();
my %results = (); # hash { config file } = ( protexid => [ logfile, server, total ] );

@CFG = split(/,/,join(',',@CFG)); # split the config file and add back into the same array

# Enable multiple configuration
# Within the loop, perform the following
foreach my $config (@CFG) {
    my ( $name, $path, $suffix ) = fileparse( $config, qr/\.[^.]*/ );
    @bom = (); # reset bom array
    $logfile = "$pwd/bdscan-$name-$timestamp.log";
    &printHelp ( 0, 0, "Processing '$config'" );
    &main($config);
}

# sent the email here
# the email require recipient, logfile, $pip_url, $pip_projid, scan config, total scan

# We're almost there, construct mail here
# first, get the content we need from log files
my ( $mail_header, $mail_body, $mail_footer );   # hold the constructed email static text
my ( @mail_attachments, @mail_recipients ) = ();

#
# For debugging only
#
if ( $DEBUG ) {
  for my $result ( keys %results ) {
    &printHelp ( 0, 0, "-" x 50 );
    &printHelp ( 0, 0, "Dump Hash Content: $result " );
	&printHelp ( 0, 0, "-" x 50 );
      for my $i ( 0 .. $#{ $results{$result} } ) {
        &printHelp ( 0, 0, " $i = $results{$result}[$i]" );
      }
    &printHelp ( 0, 0, "-" x 50 );
	&printHelp ( 0, 0, "-" x 50 );
  }
}

$mail_header = qq{
 <html lang="en">
  <head>
    <meta content="text/html; charset=utf-8" http-equiv="Content-Type">
    <title>
      ProtexIP
    </title>
    <style type="text/css">
    a:hover { text-decoration: none !important; }
    .header h1 {color: #47c8db; font: bold 32px Helvetica, Arial, sans-serif; margin: 0; padding: 0; line-height: 40px;}
    .header p {color: #c6c6c6; font: normal 12px Helvetica, Arial, sans-serif; margin: 0; padding: 0; line-height: 18px;}

    .content h2 {color:#646464; font-weight: bold; margin: 0; padding: 0; line-height: 26px; font-size: 18px; font-family: Helvetica, Arial, sans-serif;  }
    .content p {color:#767676; font-weight: normal; margin: 0; padding: 0; line-height: 20px; font-size: 12px;font-family: Helvetica, Arial, sans-serif;}
    .content a {color: #0eb6ce; text-decoration: none;}
    .footer p {font-size: 11px; color:#7d7a7a; margin: 0; padding: 0; font-family: Helvetica, Arial, sans-serif;}
    .footer a {color: #0eb6ce; text-decoration: none;}

    </style>    
  </head>
  <body style="margin: 0; padding: 0; background: #ffffff" bgcolor="#ffffff">
          <table cellpadding="0" cellspacing="0" align="center" width="600" style="border-width: 1px; border-color: #000000; border-style: solid">
          <tr>
              <td align="center" style="margin: 0; padding: 0 ;" >
                <table cellpadding="0" cellspacing="0" border="0" align="center" width="600" style="font-family: Helvetica, Arial, sans-serif; background:#2a2a2a;" class="header">
                      <tr>
                        <td width="600" align="left" style="padding: font-size: 0; line-height: 0; height: 7px;" height="7" colspan="2"></td>
                      </tr>
                    <tr>
                    <td width="20"style="font-size: 0px;">&nbsp;</td>
                    <td width="580" align="left" style="padding: 18px 0 10px;">
                        <h1 style="color: #47c8db; font: bold 32px Helvetica, Arial, sans-serif; margin: 0; padding: 0; line-height: 40px;">Automated ProtexIP Scan Report</h1>
                        <p style="color: #c6c6c6; font: normal 12px Helvetica, Arial, sans-serif; margin: 0; padding: 0; line-height: 18px;">Based on Black Duck(TM) Protex</p>
                    </td>
                  </tr>
                </table><!-- header-->
};

$mail_footer = qq{
                <table cellpadding="0" cellspacing="0" border="0" align="center" width="600" style="font-family: Helvetica, Arial, sans-serif; line-height: 10px; background:#2a2a2a" class="footer"> 
                <tr>
                    <td align="center" style="padding: 5px 0 10px; font-size: 5px; color:#7d7a7a; margin: 0; line-height: 1.2;font-family: Helvetica, Arial, sans-serif;" valign="top">
                    &nbsp;
                    </td>
                  </tr>                
                <tr>
                    <td align="center" style="padding: 5px 0 10px; font-size: 11px; color:#7d7a7a; margin: 0; line-height: 1.2;font-family: Helvetica, Arial, sans-serif;" valign="top">
                        <p style="font-size: 11px; color:#7d7a7a; margin: 0; padding: 0; font-family: Helvetica, Arial, sans-serif;">Please do not reply directly to this email. It was sent from an unattended mailbox.
                        <br/>Copyright (c) 2011 by Engineering Services. Visit <a href="http://wiki.ith.intel.com/display/ecgengsrv">Wiki</a> for more information.
                        </p>
                    </td>
                  </tr>
                </table><!-- footer-->
              </td>
              </td>
        </tr>
    </table>
  </body>
</html>
};

# Process the final result and get ready for email, if require

my ( $pip_project, $pip_resultcolor, $pip_warning );

for my $item ( keys %results ) {
    my @mbody = ();                                                                           # store the scan result
    
    $pip_project      = "N/A";                                                                # default protexip projectname
    $pip_resultcolor  = "#ffffff";                                                            # default scan result - color reporting, WHITE=OK, YELLOW=NOT OK
    
    push (@mail_attachments, $results{$item}[2]);                                                # attach logfile for email
    push (@mail_recipients, split(",", &trim($results{$item}[5]))) if length $results{$item}[5] !=0;    # retrieve email address
    push (@mail_recipients, split(",", $EMAIL)) if length($EMAIL) !=0;                           # retrieve email address from the commandline
    open(LOG, $results{$item}[2]) or die ERROR_PREFIX . "Can't open '$results{$item}[2]': $!";
    while (<LOG>) {
        my $bdslog = $_;
        chomp ($bdslog);                                                               # Get rid of the trailing space
        next if $bdslog =~ m/^$/;                                                      # Skip if line empty
        next if $bdslog =~ m/^\s*#/;                                                   # Skip if line contain comment
        $bdslog =~ s/\[.+]\s//;                                                        # remove timestamp
        if ( $bdslog =~ m/^Analyzing project/ ) {
          my @tmp_values = split(" ", $bdslog);
          # retrieve the project name from the line at column 3 and 4 based on space delimiter
          # eg. Analyzing project(1) c_stan55-test(2) STAN55-TEST(3) (c_stan55-test)(4)
          my @slice_tmp_values = @tmp_values[3,4];          
          $pip_project = join(" ", @slice_tmp_values );
        }
        if ( $bdslog =~ m/^Files/ ) {
          push(@mbody, $bdslog . "<br/>");                                             # the information tha we need to show
        }
        if ( $bdslog =~ m/^Files pending/ ) {
          $pip_resultcolor = "#ffff00";
          $pip_warning = 1;
        }
    }
   close(LOG);
# Begin to construct mail mesg
$mail_body .= qq{
                    <table cellpadding="0" cellspacing="0" border="0" align="center" width="600" style="font-family: Helvetica, Arial, sans-serif; background: $pip_resultcolor;" bgcolor="#fff">
                    <tr>
                    <td width="600" valign="top" align="left" style="font-family: Helvetica, Arial, sans-serif; padding: 20px 0 0;" class="content">
                        <table cellpadding="0" cellspacing="0" border="0"  style="color: #717171; font: normal 11px Helvetica, Arial, sans-serif; margin: 0; padding: 0;" width="600">
                        <tr>
                            <td width="21" style="font-size: 1px; line-height: 1px;"></td>
                            <td style="padding: 10px 0 0;" align="left">            
                                <p style="color:#2a2a2a; font-weight: bold; margin: 0; padding: 0; line-height: 26px; font-size: 12px; font-family: Helvetica, Arial, sans-serif; ">
                                <pre>ProtexIP Project    : $pip_project</pre>
                                <pre>ProtexIP Server     : $results{$item}[3]</pre>                                
                                <pre>Scan Configuration  : $results{$item}[0]</pre>
                                <pre>Total Scanned files : $results{$item}[4]</pre>
                                <br/>
                                <pre>Status</pre>
                                </p>
                            </td>
                            <td width="21" style="font-size: 1px; line-height: 1px;"></td>
                        </tr>
                        <tr>
                            <td width="21" style="font-size: 1px; line-height: 1px;"></td>
                            <td style="padding: 10px 5 10px; background-color: #f2f2f2"  valign="top">
                                <p style="color:#2a2a2a; font-weight: normal; margin: 0; padding: 0; line-height: 20px; font-size: 12px;font-family: Helvetica, Arial, sans-serif; ">@mbody<br>
                            </td>
                            <td width="21" style="font-size: 1px; line-height: 1px;"></td>
                        </tr>
                        </table>    
                    </td>
                  </tr>
                      <tr>
                        <td width="600" align="left" style="padding: font-size: 10" height="3" colspan="2">&nbsp;</td>
                      </tr>    
                </table><!-- body -->
};
}

# Get the mail out
if ( ( defined $EMAIL ) || ( scalar(@mail_recipients) != 0 ) && ( $LISTID != 1 ) ) {
    my $email_content = $mail_header . $mail_body . $mail_footer;
    my $email_subject = ( $ENV{'BDSJOB'} ) ? "Automated ProtexIP Scan Report - $ENV{'BDSJOB'}" : "Automated ProtexIP Scan Report";
    my $email_recipients = join(",", &uniq(@mail_recipients));
    my $email_priority = ( $pip_warning ) ? "1" : "3"; # 3 - normal, 1 - urgent
    
    &printHelp ( 0, 0, "-" x 50 );
    &printHelp ( 0, 0, "Sending Email Notification ..." );
    &printHelp ( 0, 0, "-" x 50 );
    &printHelp ( 0, 0, "Recipients : $email_recipients" );
    &printHelp ( 0, 0, "Attachments: " . join(',', @mail_attachments) );
    &printHelp ( 0, 0, "-" x 50 );
    &send_mail_with_attachments( $email_recipients, $email_subject , $email_content, $email_priority, @mail_attachments );
    foreach my $log ( @mail_attachments ) {
         unlink $log or warn WARNING_PREFIX . "Could not unlink $log $!";
    }
    # remove log files when email flag were used
    &printHelp ( 0, 0, "-" x 50 );
    &printHelp ( 0, 0, "Log files removed" );    
    &printHelp ( 0, 0, "-" x 50 );
}

#+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
# main() routine
#+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

sub main($) {
    # Global variable
    my %config; # hash to store the pair value in configuration file
    my $cfg = shift;
    my $filepath = $cfg;

    my @ignorebom = (); #reset ignorebom array

    # Get the config file name with dir and extension
    my ( $cfgname, $cfgpath, $cfgsuffix ) = fileparse( $filepath, qr/\.[^.]*/ );
    my $pip_prefix = $PREFIX;

    # Read the config file 
    &readCfg ($cfg, \%config);

    # Retrieve all the supported config value
    # Minimum
    my $pip_url      = ( ( defined $config{protexip_url} ) and ( length $config{protexip_url} != 0 ) ) ? 
                       &ExpandVar( $config{protexip_url} ) : die ERROR_PREFIX . "'protexip_url' not defined or is empty string";
    # protexip login
    my $pip_login    = ( ( defined $config{protexip_login} ) and ( length $config{protexip_login} != 0 ) ) ? 
                       &ExpandVar( $config{protexip_login} ): die ERROR_PREFIX . "'protexip_login' not defined or is empty string";
    # protexip login passwd
    my $pip_password = ( ( defined $config{protexip_password} ) and ( length $config{protexip_password} != 0 ) ) ? 
                       &ExpandVar( $config{protexip_password} ) : die ERROR_PREFIX . "'protexip_password' not defined or is empty string";
                       
    # protexip project_id
    my $pip_projid   = ( ( defined $config{protexip_projid} ) and ( length $config{protexip_projid} != 0 ) ) ? 
                       $config{protexip_projid} : undef;
    # bomlist
    my $pip_bomlist  = ( ( defined $config{protexip_bomlist} ) and ( length $config{protexip_bomlist} != 0 ) ) ? 
                       $config{protexip_bomlist} : undef;

    # Optional
    # email list
    my $pip_email    = ( ( defined $config{email_to} ) and ( length $config{email_to} != 0 ) ) ? 
                       $config{email_to} : undef;
    # ignore bomlist
    my $pip_ignorelist = ( ( defined $config{protexip_ignorelist} ) and ( length $config{protexip_ignorelist} != 0 ) ) ? 
                       $config{protexip_ignorelist} : undef;

    # additional suffix
    my $pip_suffix = ( ( defined $config{pre_suffix} ) and ( length $config{pre_suffix} != 0 ) ) ? 
                       $config{pre_suffix} : undef;

    if ( defined $pip_suffix ) {
      $pip_prefix .= $pip_suffix;
    }

    $pip_prefix =~ tr!\\!//!; # translate the path separator to perl friendly forward slash
                       
    # list visible project id
    if ( $LISTID ) {
      &ListPIP($pip_url, $pip_login, $pip_password);
      return;
    }

    # Rename the existing log file
    if ( -e $logfile ) {
      move ( $logfile, $logfile . ".old");
    }

    # Set the protexip workspace
    # my $tempdir = tempdir(CLEANUP => !$DEBUG, DIR => $pwd); #redirect temp location
    # use TMP env variable on user session to override the tempdir 
    my $tempdir = tempdir(CLEANUP => !$DEBUG);
	# leave the tempdir if dryrun
    my ( $dest ) = ( $DRYRUN ) ?  "$pwd/.protex/$pip_projid/" : "$tempdir/.protex/$pip_projid/";

    # validate require parameter
    unless ( defined $pip_projid ) {
      &printHelp ( 0, 2, "ProtexIP ProjectID (protexip_projid) not defined" );
      exit -1;
    }

    if ( defined $pip_bomlist ) {
      &valList($pip_prefix, $cfgpath . $pip_bomlist);
    }
    else {
      &printHelp ( 0, 2, "No bomlist (protexip_bomlist) defined" );
      exit -1;
    }

    if ( defined $pip_ignorelist ) {
    # read the ignorelist and populate to array for later use
        $pip_ignorelist = $cfgpath . $pip_ignorelist;
        my $iLineCount = 0;
        &printHelp ( 0, 0, "-" x 50 );
        &printHelp ( 0, 0, "Checking '$pip_ignorelist' item ... ");
        &printHelp ( 0, 0, "-" x 50 );
        
        writeLog ( "--------------------------------------------------\n", $logfile );
        writeLog ( "Checking '$pip_ignorelist' item ...\n", $logfile);
        writeLog ( "--------------------------------------------------\n", $logfile );    
        open(IGNORE, "$pip_ignorelist") or die "Can't open '$pip_ignorelist': $!";
        while (<IGNORE>) {
            my $ignorelist = $_;
            $ignorelist =~ s/\r\n/\n/g;                                        # Convert CR/LF to LF
            chomp ($ignorelist);                                               # Get rid of the trailing space            
            next if $ignorelist =~ m/^$/;                                      # Skip if line empty
            next if $ignorelist =~ m/^\s*#/;                                   # Skip if line contain comment
            $iLineCount++;
            push(@ignorebom, $ignorelist);                                     
            &printHelp ( 0, 0, "Ignore - $ignorelist" );
            writeLog ( "Ignore - $ignorelist\n", $logfile);
        }
        close(IGNORE);
        &printHelp ( 0, 0, "-" x 50 );
        &printHelp ( 0, 0, "Total = $iLineCount item" );
        &printHelp ( 0, 0, "-" x 50 );
        writeLog ( "--------------------------------------------------\n", $logfile );
        writeLog ( "Total = $iLineCount item\n", $logfile);
        writeLog ( "--------------------------------------------------\n", $logfile );    
    }

    # Prepare protexip workspace
    &printHelp ( 0, 0, "-" x 50 );
    &printHelp ( 0, 0, "ProtexIP Workspace - $dest" );
    &printHelp ( 0, 0, "-" x 50 );
    writeLog ( "--------------------------------------------------\n", $logfile );
    writeLog ( "ProtexIP Workspace - $dest\n", $logfile);
    writeLog ( "--------------------------------------------------\n", $logfile );

    # 'try' to remove protexip workspace if exist
    if ( -d $dest ) {
        rmtree( $dest );
    }
    else {
        mkpath( $dest, 0 );
    }

    # process each item in the (generated) bomlist
    # subsitute the prefix with the dest
    foreach my $filespec (@bom)  {
        my $is_absolute = File::Spec->file_name_is_absolute( $filespec );  # ensure file/directory name is absolute
        $filespec =~ tr!\\!//!;                                            # convert the path separator to perl friendly forward slash 
        
        my $dfilespec = $filespec;                                         # made the dest same as origin first

        $dfilespec =~ s|^$pip_prefix|$dest|;                               # then we subsitute prefix with dest, no effect on prefix which is 'empty'
        $dfilespec =~ s|[^.]\S:(?=/)||;                                    # remove drive letter within the string but not at beginning, windows only

        if ( $is_absolute ) {
            &fcopy ($filespec, $dfilespec, @ignorebom);
        }
        else {
            die ERROR_PREFIX . "relative path not supported";              # this shouldn't be happening, something might goes wrong with path
        }
    }

    # Traverse the workspace
    my ( @bdstotal, $bdstotal, $patchext );
	
	# Get total files
    find(sub {push(@bdstotal, File::Spec->rel2abs( $File::Find::name ) )if -f;}, $dest); # files only, directory not needed
    
	$bdstotal = scalar(@bdstotal);
	$patchext = "\.(patch|diff)\$";

    # Post-Processing
	# Best to do it here as the original source "might" be read-only which prevent modification
	# - 1. replace leading character "+" with space in patch/diff files.
    foreach my $files (@bdstotal) {
        &printHelp ( 0, 0, $files );
        writeLog ("$files\n", $logfile);
        # In case patch/diff file was detected
		if ( $files =~ m|($patchext)|x ) {
            open (PATCH, "<", "$files") or die $!; # reading original source
            open (NEWPATCH, ">", "$files.new") or die $!; # temp for output
			while (<PATCH>) {
			  my $line = $_;
			  
			  # pattern match "+" sign at the very first character
			  if ( $line =~ m|^\+| ) {
				  eval ($line =~ s|$1| |);
			  }
			  
			  print NEWPATCH $line;
			}
		close (PATCH);
        close (NEWPATCH);
		move("$files.new", "$files");
		}
    }
    &printHelp ( 0, 0, "-" x 50 );
    &printHelp ( 0, 0, "Total: $bdstotal files" );
    &printHelp ( 0, 0, "-" x 50 );

    writeLog ( "--------------------------------------------------\n", $logfile );
    writeLog ( "Total: $bdstotal files\n", $logfile );
    writeLog ( "--------------------------------------------------\n", $logfile );

    # Why run protexip if list is empty ?
    die ERROR_PREFIX . "$bdstotal files to scan" if ( $bdstotal == 0 );

    # Start ProtexIP Execution
    &printHelp ( 0, 0, "-" x 50 );
    &printHelp ( 0, 0, "Begin ProtexIP processing ..." );
    &printHelp ( 0, 0, "-" x 50 );

    my $pip_stat = 0;
    unless ($DRYRUN) {
      $pip_stat = &RunPIP($dest, $pip_url, $pip_login, $pip_password, $pip_projid, $logfile, $FORCE);
    }

    # store the information in hash
    # hash { config file } = ( cfgfile => [ projectid, logfile, server, total, email ] );
    $results{$cfg} = [ $cfgname.$cfgsuffix, $pip_projid, $logfile, $pip_url, $bdstotal, $pip_email ];

    # perl boolean true refer to non-zero
    if ( ! $pip_stat ) {
        # switch back to original location to remove the intermediate directory
        # which is now handle by random tempdir generator
        chdir ( $pwd );
        &printHelp ( 0, 0, "-" x 50 );
        &printHelp ( 0, 0, "ProtexIP Scan Completed");
        &printHelp ( 0, 0, "Log - $logfile");
    }
    else {
        &printHelp ( 0, 0, "-" x 50 );
        &printHelp ( 0, 2, "Error during ProtexIP Scan, please check $logfile");
        exit (-1);
    }
}

#+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
# List Visible ProtexIP Project ID
#+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
sub ListPIP($$$) {
    my ( $pipurl, $piplogin, $pippasswd ) = @_;
    my ( $bdscmd );
    
    # login into protexip
    &printHelp ( 0, 0, "Login into ProtexIP - $pipurl" );
    &printHelp ( 0, 0, "-" x 50 );
    $bdscmd = "bdstool login --server $pipurl --user $piplogin --password $pippasswd";
    &printHelp ( 0, 0, "CMD: $bdscmd" ) if $DEBUG;
    &RunCMD($bdscmd);
    
    # run protexip
    &printHelp ( 0, 0, "-" x 50 );    
    &printHelp ( 0, 0, "Available ProtexIP Project ..." );
    &printHelp ( 0, 0, "-" x 50 );
    $bdscmd = "bdstool --verbose list-projects";
    &printHelp ( 0, 0, "CMD: $bdscmd" ) if $DEBUG;    
    &RunCMD($bdscmd);
    
    # logout
    &printHelp ( 0, 0, "-" x 50 );    
    &printHelp ( 0, 0, "Logout from ProtexIP - $pipurl" );
    &printHelp ( 0, 0, "-" x 50 );
    $bdscmd = "bdstool logout";
    &printHelp ( 0, 0, "CMD: $bdscmd" ) if $DEBUG;
    &RunCMD($bdscmd);
}

#+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
# Run ProtexIP Analyze
#+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

sub RunPIP($$$$$) {
    my ( $workspace, $pipurl, $piplogin, $pippasswd, $pipprojid, $piplog, $force ) = @_;
    my ( $bdscmd, $bdstat, $bdscmd_arg );
    $bdscmd_arg = "--project $workspace";
    
    # switch to workspace
    # chdir ( $workspace );
    
    # login into protexip
    &printHelp ( 0, 0, "Login into ProtexIP - $pipurl" );
    &printHelp ( 0, 0, "-" x 50 );
    $bdscmd = "bdstool $bdscmd_arg login --server $pipurl --user $piplogin --password $pippasswd";
    &printHelp ( 0, 0, "CMD: $bdscmd" ) if $DEBUG;
    &RunCMD($bdscmd, $piplog);

    # associate project
    &printHelp ( 0, 0, "-" x 50 );
    &printHelp ( 0, 0, "Associating ProtexIP Project '$pipprojid'" );
    &printHelp ( 0, 0, "-" x 50 );
    $bdscmd = "bdstool $bdscmd_arg new-project $pipprojid";
    &printHelp ( 0, 0, "CMD: $bdscmd" ) if $DEBUG;
    &RunCMD($bdscmd, $piplog);
    
    # run protexip
    &printHelp ( 0, 0, "-" x 50 );    
    &printHelp ( 0, 0, "Executing Analysis ..." );
    &printHelp ( 0, 0, "-" x 50 );
    # force, 0 = false, 1 = true, default is true
    $bdscmd = ( $force ) ? "bdstool $bdscmd_arg --verbose analyze --force" : "bdstool $bdscmd_arg --verbose analyze";
    &printHelp ( 0, 0, "CMD: $bdscmd" ) if $DEBUG;    
    &RunCMD($bdscmd, $piplog);

    # logout
    &printHelp ( 0, 0, "-" x 50 );    
    &printHelp ( 0, 0, "Logout from ProtexIP - $pipurl" );
    &printHelp ( 0, 0, "-" x 50 );
    $bdscmd = "bdstool $bdscmd_arg logout";
    &printHelp ( 0, 0, "CMD: $bdscmd" ) if $DEBUG;
    &RunCMD($bdscmd, $piplog);
    
    return $bdstat;
}

#+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
# RunCMD() routine
#+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

sub RunCMD($$) {
    my ( $cmd ) = shift;
    my ( $cmdlog ) = shift;
    my ( $cmdout );
    
    # execute command
    open(COMMAND, "$cmd |") or die "can't fork $cmd: $!";
    while (<COMMAND>) {        # do something with input
        $cmdout = $_;
        print $cmdout;
        writeLog ($cmdout, $cmdlog) if ( length $cmdlog != 0 );
    }
    close(COMMAND) or die "can't close $cmd: $!, please check the command error";
}

#+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
# ProcessCmdLineOptions() routine
#+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

sub ProcessCmdLineOptions(@) {
    my @cmdlineoptions = @_;
    my $cmdlen = scalar @ARGV;
    my $rc = GetOptions(@cmdlineoptions); # rc= if argument error, else rc=1
    my $rcStat = 0;                       # return status with default false

    # no parameter specify
    if ( (not $cmdlen) ) {
        &printHelp ( 1, 2, "No argument provided ..." );
        exit (-1);
    }
    
    # invalid argument
    if ( (not $rc) ) {      
        &printHelp ( 1, 2, "Invalid argument ..." );
        exit (-1);
    }
    
    # help defined
    if ( $HELP ) {
        &printHelp ( 1, 3, "" );
        exit (0);
    }
    
    if ( $LISTID ) {
        &printHelp ( 0, 0, "List existing ProtexIP project" );
        $rcStat = 1;
        return $rcStat;
    }    

    # pre-requisite
    if ( not @CFG ) {
        &printHelp ( 1, 2, "Missing cfg parameter ..." );
    }    
    elsif ( not defined $PREFIX ) {
        &printHelp ( 1, 2, "Missing prefix parameter ..." );
    }    
    else {
        &printHelp ( 0, 0, "-" x 50 );
        &printHelp ( 0, 0, "PREFIX : $PREFIX ");
        &printHelp ( 0, 0, "CFG    : @CFG");
        &printHelp ( 0, 0, "EMAIL  : $EMAIL") if ( defined $EMAIL );
        &printHelp ( 0, 0, "-" x 50 );
        $rcStat = 1;     
    }
    
    return $rcStat;
}

#+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
# valList function to validate the bomlist
#+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

sub valList(@) {
    my ( $pref, $bomfile ) = @_;
    &printHelp ( 0, 0, "-" x 50 );
    &printHelp ( 0, 0, "Checking '$bomfile' content ... ");
    &printHelp ( 0, 0, "-" x 50 );
    
    writeLog ( "--------------------------------------------------\n", $logfile );
    writeLog ( "Checking '$bomfile' content\n", $logfile);
    writeLog ( "--------------------------------------------------\n", $logfile );
    
    open(INFILE, "$bomfile") or die ERROR_PREFIX . "Can't open '$bomfile': $!";
    my @bomlist = <INFILE>;
    my $lineCount = 0;

    foreach my $item (@bomlist) {
      $item =~ s/\r\n/\n/g;                       # Convert CR/LF to LF
      chomp ($item);      
      next if $item =~ m/^$/;                     # Skip if empty line
      next if $item =~ m/^\s*#/;                  # Skip if line contain comment
      my $line = $item;
      
      $line = $pref . $item;                      # concatenate prefix and bom item to built the absolute pathname
      
      if (-e $line)  {                            # check file/folder existent
        &printHelp ( 0, 0, "$line - Exist" );
        writeLog ("Exist - $line\n", $logfile);
        $lineCount++;
        push(@bom, $line);
      }
      else {
	    &printHelp ( 0, 2, "$line - No Such File or Directory" );
        writeLog (ERROR_PREFIX . "$line - No Such File or Directory\n", $logfile); 
        exit -1;
      }
    }
    # close file handler
    close(INFILE);
    # print total
    &printHelp ( 0, 0, "-" x 50 );
    &printHelp ( 0, 0, "Total = $lineCount item" );
    &printHelp ( 0, 0, "-" x 50 );
    writeLog ( "--------------------------------------------------\n", $logfile );
    writeLog ( "Total = $lineCount item\n", $logfile);
    writeLog ( "--------------------------------------------------\n", $logfile );
}

#+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
# readCfg function to read external config
#+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

sub readCfg(@) {    
    my ($cfgFile, $cfgHash) = @_;
    my ($cfgLine, $cfgName, $cfgValue);

    open(INFILE, "$cfgFile") or die "Can't open '$cfgFile': $!";
    
    while (<INFILE>) {
        $cfgLine = $_;
        $cfgLine =~ s/\r\n/\n/g;                                # Convert CR/LF to LF        
        chomp ($cfgLine);                                       # Get rid of the trailing space
        next if $cfgLine =~ m/^$/;                              # Skip if line empty
        next if $cfgLine =~ m/^\s*#/;                           # Skip if line contain comment
        ($cfgName, $cfgValue) = split (/=/, $cfgLine, 2);       # Split each line into name value pairs and limit to 2 fields
        $cfgName = trim($cfgName);
        $cfgValue = trim($cfgValue);
        $$cfgHash{$cfgName} = $cfgValue;                        # Assign value pairs into hash        
    }
    close(INFILE);
    
    if ( $DEBUG ) {
      my $config_key;
      my %cfgHash_ref = %$cfgHash;
      
      &printHelp ( 0, 0, "-"x50 );
      &printHelp ( 0, 0, uc("$cfgFile"));
      &printHelp ( 0, 0, "-"x50 );
      writeLog ( "--------------------------------------------------\n", $logfile ) if $DEBUG;
      writeLog ( "Configuration - '$cfgFile' \n", $logfile );
      writeLog ( "--------------------------------------------------\n", $logfile ) if $DEBUG;
      
      foreach $config_key (sort (keys %cfgHash_ref)) {
        &printHelp (0, 0, "$config_key = $cfgHash_ref{$config_key}");
      }
      
      &printHelp ( 0, 0, "-"x50 );
    }    
    return 0;
}

#+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
# remove leading and trailing space function
#+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

sub trim($) {
    my $string = shift;
    $string =~ s/^\s+//;
    $string =~ s/\s+$//;
    return $string;
}

#+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
# remove duplicate in list
#+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
sub uniq($) {
    my %seen = ();
    my @r = ();
    foreach my $a (@_) {
        unless ($seen{$a}) {
            push @r, $a;
            $seen{$a} = 1;
        }
    }
    return @r;
}

#+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
# Expand variable by substituting any Environment variables in it
#+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

sub ExpandVar($) {
    my $string = shift;

    # nothing to expand, return the original string
    return $string unless ( $string =~ /\$[a-z]/i );

    my @envKey = $string =~ /\$ENV\{([^}]+)}/gx;
    # sort the existing env
    my @keys = sort { length $b <=> length $a } keys %ENV;

    # iterate through existing system env
    for my $key ( @keys ) {
       my $value = $ENV{$key};
       &printHelp( 0, 0, "Processing $key, $value" ) if $DEBUG;
       
       # iterate matched ENV value
       foreach my $strKey ( @envKey ) {
         if ( $strKey =~ /^$key$/ and $value ne "" ) {
           $string =~ s/\$ENV{($strKey)}/$value/;
           &printHelp( 0, 0, "Environment expanded $key: $value" ) if $DEBUG;
           &printHelp( 0, 0, "String: $string" ) if $DEBUG;
         }
       }
    }
    
    # env doesn't get expanded
    if ( $string =~ /\$ENV\{([^}]+)}/) {
          die ERROR_PREFIX . "Check environment. Expansion did not find \$ENV{$1}";
    }

    return $string;
}

#+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
# recursive directory scan subroutine
#+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

sub gen_bom_from_dir() {
  # convert to absolute path;
  my $file = $File::Find::name;
  push(@bom, File::Spec->rel2abs( $file ));

  print File::Spec->rel2abs( $file ) . "\n" if $DEBUG;
}

#+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
# recursive copy subroutine
#+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

# the recursive copy might copy the file twice
# this is expected in order to support mixed format of bomlist file

sub fcopy($$$) {
    my $from = shift(@_); # src to copy
    my $to = shift(@_);   # dest to paste
    my @filter = @_;      # regex to filter, in array reference
    
    my $parentdir;
    my $abort = 0;

    # filter the from item based on the regex
    if ( scalar @filter != 0 ) {
        foreach my $ignore_item (@filter) {
            if ( $from =~ m|($ignore_item)| ) {
              $abort = 1;
              &printHelp ( 0, 0, "Ignoring '$from' due to '$ignore_item' regex");
              writeLog ( "Ignoring '$from' due to '$ignore_item'\n", $logfile );
              last; # don't waste time to scan the rest of ignore list
            }
        }
    }
    
    return if $abort;
	
    if ( -d $from ) {
        if ( not -d $to ) {       # if $to dir doesn't exist
                &printHelp( 0, 0, "mkpath $to" ) if $DEBUG;
                eval { mkpath ( $to, 0 ); };
                die ERROR_PREFIX . "mkpath '$to': $! ($^E)" if $@;
        }
        opendir DIR, $from;
        my @entries = readdir DIR;
        for (@entries) {
                next if /^\.\.?$/; # skip hiddent dot
                &fcopy ("$from/$_", "$to/$_", @filter);
        }
        closedir DIR;
    } 
    else {
        $parentdir = dirname($to);
        if ( not -d $parentdir )  {          # if $to parent dir doesn't exist
            &printHelp ( 0, 0, "mkpath $parentdir" ) if $DEBUG;
            eval { mkpath ($parentdir, 0 ); };
            die ERROR_PREFIX . "mkpath '$parentdir': $! ($^E)" if $@;
        }
        -f $from or die ERROR_PREFIX . "Not dir/plain file: '$from'";
        # may not be a good idea to enable verbose display by default for those large codebase
        &printHelp ( 0, 0, "copying $from => $to" ) if $DEBUG;
        unless ( copy_file_bin ( $from, $to ) ) { die ERROR_PREFIX . "Copy failed $from => $to : $!" };
    }
}

#+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
# single file copy function
#+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

sub copy_file_bin {
    my ($src, $dst) = @_;
    my $buf;

    open(COPYIN, "<$src")  or warn "Can't read $src: $!\n";
    open(COPYOUT, ">$dst") or warn "Can't write $dst: $!\n";
    binmode COPYIN;
    binmode COPYOUT;

    while ( read(COPYIN, $buf, 65536) and print COPYOUT $buf ) {};

    close COPYOUT || warn "Can't close $dst: $!\n";
    close COPYIN  || warn "Can't close $src: $!\n";
}

#+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
# logging function
#+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

sub writeLog($$) {
    # Time and date stamp
    my ($sec,$min,$hour,$mday,$mon,$year,$wday,$yday,$isdst) = localtime(time);
    my ( $mesg, $log ) = @_;
    open(FILE, ">> $log") or die $!;
        printf FILE "[%04d/%02d/%02d %02d:%02d:%02d] $mesg",$year+1900,$mon+1,$mday,$hour,$min,$sec;
    close(FILE);
}

#+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
# sendmail function
#+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

# Example
# &send_mail_with_attachments(emailaddress, subject, body, logfile1, logfile2, ...);
# &send_mail_with_attachments('someone\@company.com', 'subject', 'body', 'location of attachment','location of attachment', ...);

sub send_mail_with_attachments {
    my $to = shift(@_);
    my $subject = shift(@_);
    my $body = shift(@_);
    my $priority = shift(@_);
    my @attachments = @_;

    my $from = "chesbuild\@intel.com";
    my $smtp;

    if (not $smtp = Net::SMTP->new('smtp.intel.com',
                               Debug => $DEBUG)) {
     die "Could not connect to server\n";
    }

    # Create arbitrary boundary text used to separate
    # different parts of the message
    my ($bi, $bn, @bchrs);
    my $boundry = "";
    foreach $bn (48..57,65..90,97..122) {
      $bchrs[$bi++] = chr($bn);
    }
    foreach $bn (0..20) {
      $boundry .= $bchrs[rand($bi)];
    }

    # Send the header
    $smtp->mail($from . "\n");
    my @recepients = split(/,/, $to);
    foreach my $recp (@recepients) {
      $smtp->to($recp . "\n");
    }
    # If you need to CC more email
    #$smtp->cc("others\@company.com");
    $smtp->data();
    $smtp->datasend("From: " . $from . "\n");
    $smtp->datasend("To: " . $to . "\n");
    $smtp->datasend("Subject: " . $subject . "\n");
    $smtp->datasend("MIME-Version: 1.0\n");
    $smtp->datasend("Content-Type: multipart/mixed; BOUNDARY=\"$boundry\"\n");

    # this is meant to add the urgent mark ! on email
    $smtp->datasend("X-Priority: $priority\n");    
    
    # Send the body
    $smtp->datasend("\n--$boundry\n");
    $smtp->datasend("Content-Type: text/html; charset=\"us-ascii\"\n");
    $smtp->datasend("\n");
    $smtp->datasend($body . "\n\n");

    # Send attachments
    foreach my $file (@attachments) {
      unless (-f $file) {
          die "Unable to find attachment file $file\n";
          next;
      }
      my($bytesread, $buffer, $data, $total);
      open(FH, "$file") || die "Failed to open $file\n";
      binmode(FH);
      while (($bytesread = sysread(FH, $buffer, 1024)) == 1024) {
         $total += $bytesread;
         $data .= $buffer;
      }
      if ($bytesread) {
         $data .= $buffer;
         $total += $bytesread;
      }
      close FH;

      # Get the file name without its directory
      my ($volume, $dir, $fileName) = File::Spec->splitpath($file);
  
      # Try and guess the MIME type from the file extension so
      # that the email client doesn't have to
      my $contentType = guess_media_type($file);
  
      if ($data) {
         $smtp->datasend("--$boundry\n");
         $smtp->datasend("Content-Type: $contentType; name=\"$fileName\"\n");
         $smtp->datasend("Content-Transfer-Encoding: base64\n");
         $smtp->datasend("Content-Disposition: attachment; filename=\"$fileName\"\n\n");
         $smtp->datasend(encode_base64($data));
      }
    }

   # Quit
   $smtp->datasend("\n--$boundry--\n"); # send boundary end message
   $smtp->datasend("\n");
   $smtp->dataend();
   $smtp->quit;
}

#+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
# Diplay usage and help routine
#+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

sub printHelp($$$) {
    my ( $show_help, $msg_type, $msg ) = @_;
    my $fh = \*STDERR;
    my $prefix;
    
    if ($msg_type == MSG_ERR) {
        $prefix .= ERROR_PREFIX;
    }
    elsif ($msg_type ==  MSG_WARN) {
        $prefix .= WARNING_PREFIX;
    }
    elsif ($msg_type == MSG_OTHER) {
        $prefix .= "";
        $fh = \*STDOUT;    
    }
    else {                                        # msg_type == MSG_OK
        $prefix .= INFO_PREFIX;
        $fh = \*STDOUT;
    }
    print { $$fh } $prefix . $msg . "\n";

    if ( $show_help )
    {
        (my $this_prog = $0) =~ s/^.*[\\\/]//;

        print( "\n$this_prog [-help] [-prefix <path>] [-cfg <cfg file>,<cfg file>...]" .
               " [-force] [-listid] [-email] [-dryrun] [-debug]\n\n" );

        print( " -h | -help        (Display this Help Message)\n" .
               " -prefix           (Path to insert for list of files/folder defined in bomlist)\n".
               " -cfg              (Configuration file(s) which store the config parameter)\n".
               " -force            (Force protex to re-analyze all files, regardless of prior scans)\n".
               " -listid           (List ProtexIP ProjectID)\n".
               " -email            (Optional: Sent execution log via email)\n".
               "                   (set BDSJOB env var for the email subject)\n".
               "\n"
               );
        print( "Pre-requisite:\n".
               "--------------\n".
               "- ProtexIP client installed\n".
               "- Able to execute 'bdstool' on command line\n"
               );
    }

  return 0;
}
