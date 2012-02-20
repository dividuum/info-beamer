#!/usr/bin/perl

use 5.010;
use strict;
use warnings;
use bignum;
use Carp;
use SNMP::Info;
use Data::Dumper;
use Net::OpenSoundControl::Client;
use Time::HiRes qw(sleep);
use Math::BigFloat;
use List::Util qw(min);

my $sleep_time = 1;
my $snmp = SNMP::Info->new(AutoSpecify => 0,
                           Debug => 0,
                           DestHost => '10.0.0.254',
                           Community => 'public',
                           Version => 2,
                           BigInt => 1)
  or carp "cannot connect to snmp service";

my $osc_client = Net::OpenSoundControl::Client->new(
                                                    Host => '192.168.23.20',
                                                    Port => 4444)
  or carp "could not initialize osc";

my @interface_whitelist = ('alicedsl','qscdsl','eth2');

my @interfaces = grep {
  $_->{name} ~~ @interface_whitelist and
    not exists $snmp->if_ignore()->{$_->{idx}} and
      $snmp->i_up()->{$_->{idx}} eq 'up' and
        $snmp->i_up_admin()->{$_->{idx}} eq 'up';
} map {
    {idx=>$_, name=>$snmp->i_name()->{$_}}
} keys $snmp->interfaces();

my %override_max_speed = (
                          'wan' => 17_000_000,
                          'alicedsl' => 17_000_000
                         );

my %speeds = map {
    (
     $_->{name} =>
     ($override_max_speed{$_->{name}} // $snmp->i_speed_raw()->{$_->{idx}}
      or croak("no speed for interface ".$_->{name}))/8
    )
} @interfaces;

sub derive_val  {
  state %cache;
  my ($name, $idx) = @_;
  my $counter = $snmp->i_octet_in64()->{$idx} + $snmp->i_octet_out64()->{$idx};
  my $r = ($counter - ($cache{$idx} // $counter)) / $sleep_time;
  $cache{$idx} = $counter;
  return min(1,$r/$speeds{$name});
}

sub send_val {
  my $val = shift;
  $osc_client->send($val);
  say join ', ', @$val;
}

while (1) {
  eval {
    $snmp->load_i_octet_in64();
    $snmp->load_i_octet_out64();
    map {
      send_val(['/gauge/if/' . $_->{name},
                'f',
                derive_val($_->{name}, $_->{idx})]);
    } @interfaces;
    1;
  } or do {
    print STDERR "error sending counter values: " . $@;
  };
  sleep($sleep_time);
}

