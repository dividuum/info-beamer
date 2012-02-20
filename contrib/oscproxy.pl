#!/usr/bin/perl

use 5.010;
use strict;
use warnings;
use Carp;
use Data::Dumper;

use Net::OpenSoundControl::Server;
use Net::OpenSoundControl::Client;


my $client = Net::OpenSoundControl::Client->new(
    Host => $ARGV[0], Port => 4444)
    or carp "error creating osc client";

my $server = Net::OpenSoundControl::Server->new(
    Port => 4444, Handler => \&proxy) or
    carp "error creating osc server";

sub proxy {
    eval {
        my ($sender, $msg) = @_;
        $msg = [$ARGV[1] . $msg->[0], @$msg[1..2]];
        say Dumper($msg);
        $client->send($msg);
    } or do {
        say STDERR "error: $@";
    };
}

$server->readloop();

