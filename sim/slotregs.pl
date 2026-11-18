#!/usr/bin/perl
# Part of the simulation, see run.sh: reads what "VBoxManage debugvm <vm> info
# pci verbose" prints (on stdin), finds the PCI Express port whose secondary
# bus has the number given, and tells in one line what the guest has made of
# its slot: the slot registers as the guest left them.
#
#   slotregs.pl <secondary bus>
#
# The line starts with "SLOT" and has name=value pairs, among them
#   taken=1   the guest has switched on the hot-plug interrupt of the slot and
#             at least one of the events it is for (button, presence, link):
#             its hot-plug driver runs the slot
# Exit code 0 if the port was found and has a slot, 1 if not.

use strict;
use warnings;

my $want = @ARGV && $ARGV[0] =~ /^[0-9]+$/ ? $ARGV[0] + 0 : 1;

# The devices listed: address, name, config space bytes.
my @devs;
my $cur;
while (my $line = <STDIN>) {
    $line =~ s/[\r\n]+$//;
    if ($line =~ /^\s*([0-9a-f]{2}:[0-9a-f]{2}\.[0-7]) (.+?): [0-9a-f]{4}-[0-9a-f]{4} /) {
        my ($addr, $name) = ($1, $2);
        $name =~ s/ \(PASSTHROUGH\)$//;
        $cur = { addr => $addr, name => $name, cfg => [] };
        push @devs, $cur;
    }
    elsif ($cur && $line =~ /^\s*((?:[0-9a-f]{2} ){16})\s*$/ && @{$cur->{cfg}} < 256) {
        push @{$cur->{cfg}}, map { hex } split ' ', $1;
    }
}

sub u16 { my ($c, $o) = @_; return $c->[$o] | $c->[$o + 1] << 8; }
sub u32 { my ($c, $o) = @_; return u16($c, $o) | u16($c, $o + 2) << 16; }

# The capabilities in the first 256 bytes: id => offset.
sub capabilities {
    my ($c) = @_;
    my %caps;
    return %caps unless u16($c, 0x06) & 0x10;
    my $off = $c->[0x34] & 0xfc;
    my %seen;
    while ($off >= 0x40 && $off <= 0xfc && !$seen{$off}) {
        $seen{$off} = 1;
        $caps{$c->[$off]} = $off unless exists $caps{$c->[$off]};
        $off = $c->[$off + 1] & 0xfc;
    }
    return %caps;
}

my $found = 0;
for my $d (@devs) {
    my $c = $d->{cfg};
    next unless @$c == 256 && ($c->[0x0e] & 0x7f) == 1 && $c->[0x19] == $want;
    my %caps = capabilities($c);
    my $exp = $caps{0x10};
    if (!defined $exp) {
        print "SLOT port=$d->{addr} name=$d->{name} secondary=$want: no PCI Express capability\n";
        next;
    }
    my $flags = u16($c, $exp + 0x02);
    if (!($flags & 0x100)) {
        print "SLOT port=$d->{addr} name=$d->{name} secondary=$want: the port has no slot\n";
        next;
    }
    $found = 1;
    my $slotcap = u32($c, $exp + 0x14);
    my $slotctl = u16($c, $exp + 0x18);
    my $slotsta = u16($c, $exp + 0x1a);
    my $lnksta  = u16($c, $exp + 0x12);
    my $msi     = $caps{0x05};
    my $hpie    = $slotctl >> 5 & 1;
    my $abpe    = $slotctl & 1;
    my $pdce    = $slotctl >> 3 & 1;
    my $dllsce  = $slotctl >> 12 & 1;
    my $taken   = ($hpie && ($abpe || $pdce || $dllsce)) ? 1 : 0;
    printf "SLOT port=%s name=%s secondary=%d porttype=%d slotcap=0x%08x hotplug=%d surprise=%d button=%d powerctl=%d "
         . "slotctl=0x%04x hpie=%d abpe=%d pdce=%d ccie=%d dllsce=%d poweroff=%d "
         . "slotsta=0x%04x present=%d lnksta=0x%04x linkup=%d cmd=0x%04x msi=%s intpin=%d taken=%d\n",
           $d->{addr}, $d->{name}, $want, $flags >> 4 & 0xf, $slotcap, $slotcap >> 6 & 1, $slotcap >> 5 & 1, $slotcap & 1,
           $slotcap >> 1 & 1, $slotctl, $hpie, $abpe, $pdce, $slotctl >> 4 & 1, $dllsce, $slotctl >> 10 & 1,
           $slotsta, $slotsta >> 6 & 1, $lnksta, $lnksta >> 13 & 1, u16($c, 0x04),
           !defined $msi ? 'none' : (u16($c, $msi + 2) & 1 ? 'on' : 'off'), $c->[0x3d], $taken;
}
exit($found ? 0 : 1);
