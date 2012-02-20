#!/usr/bin/perl

use 5.010;
use strict;
use warnings;
use Carp;
use SNMP::Info;
use Data::Dumper;
use Net::OpenSoundControl::Client;
use Time::HiRes qw(sleep);

use bignum;

my $sleep_time = 1;
my $snmp = SNMP::Info->new(AutoSpecify => 0,
                           Debug => 0,
                           DestHost => '192.168.24.1',
                           Community => 'public',
                           Version => 2,
                           BigInt => 1)
  or carp "cannot connect to snmp service";

my $osc_client = Net::OpenSoundControl::Client->new(
                                                    Host => '192.168.23.20',
                                                    Port => 4444)
  or carp "could not initialize osc";

my $interfaces = $snmp->interfaces();

my %override_max_speed = (
                          'wan' => 17_000_000
                         );

my @interface_whitelist = ( 'entropia-wlan', 'entropia-eth', 'entropia-clubbus',
                            'henet', 'qdsl', 'entropia-voip' );

sub derive_val  {
  state %cache;
  my ($name, $idx) = @_;
  my $counter = $snmp->i_octet_in64()->{$idx} + $snmp->i_octet_out64()->{$idx};
  my $speed = $override_max_speed{$name} // $snmp->i_speed_raw()->{$idx} or carp "no interface speed available";
  my $r = ($counter - ($cache{$idx} // $counter)) / $sleep_time;
  $cache{$idx} = $counter;
  return ($r <= 0) ? 0 : log($r)/log($speed);
}

sub send_val {
  my $val = shift;
  $osc_client->send($val);
  say $val->[0] . ": " . $val->[2];
}
  
  while (1) {
    eval {
      $snmp->load_i_octet_in64();
      $snmp->load_i_octet_out64();
      map {
        send_val(['/gauge/if/' . $_->{name}, 'f', derive_val($_->{name}, $_->{idx})]);
      } grep {
        $_->{name} ~~ @interface_whitelist and
          not exists $snmp->if_ignore()->{$_->{idx}} and
            $snmp->i_up()->{$_->{idx}} eq 'up' and
              $snmp->i_up_admin()->{$_->{idx}} eq 'up';
      } map {
        {idx => $_, name => $snmp->i_name()->{$_}}
      } sort keys $interfaces;
      1;
    } or do {
      print STDERR "error sending counter values: " . $@;
    };
    sleep($sleep_time);
  }

