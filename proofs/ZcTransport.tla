---------------------------- MODULE ZcTransport ----------------------------
(***************************************************************************)
(* The zcrudp video transport as stream_live.c runs it (ordered receiver). *)
(*                                                                         *)
(* Sender (run_sender, sender_poll_ctrl, zc_video_tx_resend):              *)
(*   - frame k goes out at k * Period, all its fragments at once;          *)
(*   - a NACK (ZCNK) resends the listed fragments, or the whole frame when *)
(*     the list is empty; a ZCSY makes the next KfBurst frames keyframes;  *)
(*   - after the last frame it lingers Linger ms answering requests, and   *)
(*     resends the last fragment of the last frame at ProbeGap, 2*ProbeGap,*)
(*     3*ProbeGap (tail probe).                                            *)
(* Receiver (zc_video_rx_feed / _next / _build_nack, run_receiver):        *)
(*   - a datagram of frame f makes the unseen frames between the last one  *)
(*     delivered and f "gap" slots (rx_note_gap, only once one frame has   *)
(*     been delivered, and only up to 8 ahead);                            *)
(*   - frames leave in order; the oldest incomplete one blocks the others  *)
(*     until it has been silent for Hold ms, then it is given up;          *)
(*   - an incomplete frame is asked for NackDelay ms after it was first    *)
(*     seen, then at most every NackInt ms: the holes below the highest    *)
(*     fragment seen while it keeps arriving, everything missing once it   *)
(*     has been silent NackDelay ms (a gap slot: the whole frame);         *)
(*   - a frame given up, or a first frame that is not a keyframe, sets     *)
(*     need_sync: ZCSY twice, every SyncEvery ms, until a keyframe arrives.*)
(*                                                                         *)
(* Time is discrete (1 unit = 1 ms in the main configurations). Each       *)
(* program checks its timers in a loop that polls every 1 ms, so the timed *)
(* actions are urgent: the clock does not advance while one is due. A      *)
(* datagram is delivered or lost within Lat units of being sent, in any    *)
(* order. At most MaxLoss datagrams (data or control) are lost in all.     *)
(*                                                                         *)
(* Not modelled: FEC parity (disabled until losses are seen, and it can    *)
(* only complete frames earlier), pacing (a frame goes out in one instant),*)
(* the 32-slot and 64-frame history limits (N is far below both), the     *)
(* rate control and ZCAC acknowledgements (they carry no reliability).     *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS N,          \* frames sent
          F,          \* fragments per frame
          Period,     \* frame period
          NackDelay, NackInt, Hold,
          Linger, ProbeGap, Probes,
          SyncEvery, KfBurst,
          Lat,        \* maximum network latency
          MaxLoss,
          GapWin,     \* rx_note_gap: frames further ahead are not held for (8)
          SeqCheck    \* the fix: a non-keyframe that does not follow the
                      \* last one delivered requests a resync

Frames == 0 .. N - 1
Frags  == 0 .. F - 1
Never  == -1

VARIABLES now, net, losses,
          \* sender
          sent, kfLeft, kfSent, lingerStart, probes, sdone,
          \* receiver, per frame
          st, have, firstT, lastRx, lastNack, hi,
          \* receiver, global
          lastC, drops, prevDrops, needSync, lastSync, seenFirst, gotPkt,
          \* ghosts: picture state and delivery order
          valid, lastDlv, badSince, orderOK

vars == <<now, net, losses, sent, kfLeft, kfSent, lingerStart, probes, sdone,
          st, have, firstT, lastRx, lastNack, hi,
          lastC, drops, prevDrops, needSync, lastSync, seenFirst, gotPkt,
          valid, lastDlv, badSince, orderOK>>

Max(a, b) == IF a > b THEN a ELSE b

DataMsg(f, i) == [k |-> "D", f |-> f, i |-> i, s |-> {}, t |-> now]

Init ==
  /\ now = 0 /\ net = {} /\ losses = 0
  /\ sent = 0 /\ kfLeft = 0 /\ kfSent = [f \in Frames |-> FALSE]
  /\ lingerStart = Never /\ probes = 0 /\ sdone = FALSE
  /\ st = [f \in Frames |-> "none"] /\ have = [f \in Frames |-> {}]
  /\ firstT = [f \in Frames |-> 0] /\ lastRx = [f \in Frames |-> Never]
  /\ lastNack = [f \in Frames |-> Never] /\ hi = [f \in Frames |-> 0]
  /\ lastC = Never /\ drops = 0 /\ prevDrops = 0 /\ needSync = FALSE
  /\ lastSync = Never /\ seenFirst = FALSE /\ gotPkt = FALSE
  /\ valid = TRUE /\ lastDlv = Never /\ badSince = Never /\ orderOK = TRUE

RxVars == <<st, have, firstT, lastRx, lastNack, hi,
            lastC, drops, prevDrops, needSync, lastSync, seenFirst, gotPkt,
            valid, lastDlv, badSince, orderOK>>
TxVars == <<sent, kfLeft, kfSent, lingerStart, probes, sdone>>

(*************************** sender ***************************************)
SendDue == ~sdone /\ sent < N /\ now = sent * Period

SendFrame ==
  /\ SendDue
  /\ kfSent' = [kfSent EXCEPT ![sent] = (sent = 0 \/ kfLeft > 0)]
  /\ kfLeft' = IF kfLeft > 0 THEN kfLeft - 1 ELSE 0
  /\ net' = net \cup {DataMsg(sent, i) : i \in Frags}
  /\ sent' = sent + 1
  /\ UNCHANGED <<now, losses, lingerStart, probes, sdone, RxVars>>

LingerDue == sent = N /\ lingerStart = Never /\ now >= N * Period

StartLinger ==
  /\ LingerDue
  /\ lingerStart' = now
  /\ UNCHANGED <<now, net, losses, sent, kfLeft, kfSent, probes, sdone, RxVars>>

ProbeDue == lingerStart # Never /\ ~sdone /\ probes < Probes
            /\ now >= lingerStart + ProbeGap * (probes + 1)

Probe ==
  /\ ProbeDue
  /\ net' = net \cup {DataMsg(N - 1, F - 1)}
  /\ probes' = probes + 1
  /\ UNCHANGED <<now, losses, sent, kfLeft, kfSent, lingerStart, sdone, RxVars>>

StopDue == lingerStart # Never /\ ~sdone /\ now >= lingerStart + Linger

Stop ==
  /\ StopDue
  /\ sdone' = TRUE
  /\ UNCHANGED <<now, net, losses, sent, kfLeft, kfSent, lingerStart, probes, RxVars>>

\* A control datagram reaching the sender (only read while it runs).
SenderGets(m) ==
  IF sdone THEN /\ net' = net \ {m} /\ UNCHANGED TxVars
  ELSE IF m.k = "N"
    THEN /\ net' = (net \ {m}) \cup
                   {DataMsg(m.f, i) : i \in (IF m.s = {} THEN Frags ELSE m.s)}
         /\ UNCHANGED TxVars
    ELSE /\ net' = net \ {m}
         /\ kfLeft' = KfBurst
         /\ UNCHANGED <<sent, kfSent, lingerStart, probes, sdone>>

(*************************** receiver *************************************)
Used(f) == st[f] \in {"gap", "part", "comp"}
Since(f) == Max(firstT[f], lastRx[f])

GapIds(g) == {x \in Frames : lastC # Never /\ g - lastC > 1 /\ g - lastC <= GapWin
                             /\ lastC < x /\ x < g /\ st[x] = "none"}

\* zc_video_rx_feed for one data datagram.
Feed(m) ==
  LET f   == m.f
      gs  == GapIds(f)
      old == lastC # Never /\ f <= lastC
      fresh == st[f] = "none"
      nh  == have[f] \cup {m.i}
  IN
  /\ gotPkt' = TRUE
  /\ IF old THEN UNCHANGED <<st, have, firstT, lastRx, lastNack, hi>>
     ELSE IF st[f] = "comp"   \* duplicate: rx_note_gap has run, nothing else
     THEN /\ st' = [x \in Frames |-> IF x \in gs THEN "gap" ELSE st[x]]
          /\ firstT' = [x \in Frames |-> IF x \in gs THEN now ELSE firstT[x]]
          /\ lastNack' = [x \in Frames |-> IF x \in gs THEN Never ELSE lastNack[x]]
          /\ UNCHANGED <<have, lastRx, hi>>
     ELSE
     /\ st' = [x \in Frames |-> IF x \in gs THEN "gap"
                                ELSE IF x = f THEN (IF nh = Frags THEN "comp" ELSE "part")
                                ELSE st[x]]
     /\ have' = [have EXCEPT ![f] = nh]
     /\ firstT' = [x \in Frames |-> IF x \in gs \/ (x = f /\ fresh) THEN now ELSE firstT[x]]
     /\ lastRx' = [lastRx EXCEPT ![f] = now]
     /\ lastNack' = [x \in Frames |-> IF x \in gs \/ (x = f /\ fresh) THEN Never
                                      ELSE lastNack[x]]
     /\ hi' = [hi EXCEPT ![f] = IF fresh \/ st[f] = "gap" THEN m.i ELSE Max(@, m.i)]
  /\ UNCHANGED <<lastC, drops, prevDrops, needSync, lastSync, seenFirst,
                 valid, lastDlv, badSince, orderOK>>

Deliver(m) ==
  /\ m \in net
  /\ IF m.k = "D"
       THEN /\ net' = net \ {m}
            /\ Feed(m)
            /\ UNCHANGED <<now, losses, TxVars>>
       ELSE /\ SenderGets(m)
            /\ UNCHANGED <<now, losses, RxVars>>

Lose(m) ==
  /\ m \in net /\ losses < MaxLoss
  /\ net' = net \ {m} /\ losses' = losses + 1
  /\ UNCHANGED <<now, TxVars, RxVars>>

\* zc_video_rx_next: the oldest frame in reassembly.
Oldest == IF \E f \in Frames : Used(f)
          THEN CHOOSE f \in Frames : Used(f) /\ \A g \in Frames : Used(g) => f <= g
          ELSE Never

NextDue == Oldest # Never /\ (st[Oldest] = "comp" \/ now - Since(Oldest) >= Hold)

RxNext ==
  /\ NextDue
  /\ LET o == Oldest IN
     IF st[o] = "comp"
     THEN LET kf == kfSent[o]
              v  == kf \/ (valid /\ o = lastDlv + 1)
          IN /\ st' = [st EXCEPT ![o] = "dlv"]
             /\ lastC' = o
             /\ orderOK' = (orderOK /\ o > lastDlv /\ have[o] = Frags)
             /\ lastDlv' = o
             /\ valid' = v
             /\ badSince' = IF valid /\ ~v THEN now ELSE badSince
             /\ seenFirst' = TRUE
             /\ needSync' = IF ~seenFirst THEN (IF o > 0 /\ ~kf THEN TRUE ELSE needSync)
                            ELSE IF kf THEN FALSE
                            ELSE IF SeqCheck /\ o # lastDlv + 1 THEN TRUE
                            ELSE needSync
             /\ UNCHANGED drops
     ELSE /\ st' = [st EXCEPT ![o] = "drop"]
          /\ drops' = drops + 1
          /\ lastC' = IF lastC = Never \/ o > lastC THEN o ELSE lastC
          /\ UNCHANGED <<orderOK, lastDlv, valid, badSince, seenFirst, needSync>>
  /\ UNCHANGED <<now, net, losses, TxVars, have, firstT, lastRx, lastNack, hi,
                 prevDrops, lastSync, gotPkt>>

DropsDue == drops > prevDrops

NoteDrops ==
  /\ DropsDue
  /\ prevDrops' = drops /\ needSync' = TRUE
  /\ UNCHANGED <<now, net, losses, TxVars, st, have, firstT, lastRx, lastNack, hi,
                 lastC, drops, lastSync, seenFirst, gotPkt,
                 valid, lastDlv, badSince, orderOK>>

\* zc_video_rx_build_nack for frame f.
Stalled(f) == now - Since(f) >= NackDelay
Holes(f) == {j \in 0 .. hi[f] - 1 : j \notin have[f]}

NackDue(f) ==
  /\ gotPkt
  /\ st[f] \in {"gap", "part"}
  /\ now - firstT[f] >= NackDelay
  /\ lastNack[f] = Never \/ now - lastNack[f] >= NackInt
  /\ Stalled(f) \/ (st[f] = "part" /\ hi[f] > 0 /\ Holes(f) # {})

Nack(f) ==
  /\ NackDue(f)
  /\ LET list == IF ~Stalled(f) THEN Holes(f)
                 ELSE IF st[f] = "gap" THEN {} ELSE Frags \ have[f]
     IN net' = net \cup {[k |-> "N", f |-> f, i |-> 0, s |-> list, t |-> now]}
  /\ lastNack' = [lastNack EXCEPT ![f] = now]
  /\ UNCHANGED <<now, losses, TxVars, st, have, firstT, lastRx, hi,
                 lastC, drops, prevDrops, needSync, lastSync, seenFirst, gotPkt,
                 valid, lastDlv, badSince, orderOK>>

SyncDue == gotPkt /\ needSync /\ (lastSync = Never \/ now - lastSync >= SyncEvery)

Sync ==
  /\ SyncDue
  /\ net' = net \cup {[k |-> "S", f |-> 0, i |-> c, s |-> {}, t |-> now] : c \in {1, 2}}
  /\ lastSync' = now
  /\ UNCHANGED <<now, losses, TxVars, st, have, firstT, lastRx, lastNack, hi,
                 lastC, drops, prevDrops, needSync, seenFirst, gotPkt,
                 valid, lastDlv, badSince, orderOK>>

(*************************** time *****************************************)
\* The session is over: the sender has stopped, no data is on the way and
\* nothing is in reassembly. (Only ZCSY could still be sent; the receiver
\* repeats it forever while out of sync, with nobody to answer.)
Terminated == sdone /\ (\A m \in net : m.k # "D") /\ \A f \in Frames : ~Used(f)

Urgent == SendDue \/ LingerDue \/ ProbeDue \/ StopDue \/ NextDue \/ DropsDue
          \/ SyncDue \/ \E f \in Frames : NackDue(f)

Tick ==
  /\ ~Terminated /\ ~Urgent
  /\ \A m \in net : now - m.t < Lat
  /\ now' = now + 1
  /\ UNCHANGED <<net, losses, TxVars, RxVars>>

Done == Terminated /\ ~Urgent /\ net = {} /\ UNCHANGED vars

Next ==
  \/ SendFrame \/ StartLinger \/ Probe \/ Stop
  \/ RxNext \/ NoteDrops \/ Sync \/ \E f \in Frames : Nack(f)
  \/ \E m \in net : Deliver(m) \/ Lose(m)
  \/ Tick
  \/ Done

Spec == Init /\ [][Next]_vars /\ WF_vars(Next)

(*************************** properties ***********************************)
TypeOK ==
  /\ now \in Nat /\ losses \in 0 .. MaxLoss
  /\ st \in [Frames -> {"none", "gap", "part", "comp", "dlv", "drop"}]
  /\ \A f \in Frames : have[f] \subseteq Frags

\* Frames leave strictly in order, and only complete.
InOrder == orderOK

\* A frame is never both delivered and given up, and nothing is left
\* waiting once the session is over.
Consistent == \A f \in Frames : st[f] = "dlv" => f <= lastC

\* Every frame delivered (checked with small MaxLoss).
AllDelivered == Terminated => \A f \in Frames : st[f] = "dlv"

\* The same, except frames lost whole before the first delivery.
DeliveredAfterHead ==
  Terminated => \A f \in Frames : st[f] = "dlv"
                  \/ (st[f] = "none" /\ \A g \in Frames : g < f => st[g] # "dlv")

\* After the picture goes bad, the stream recovers it, provided the sender
\* still sends frames for ResyncWindow units afterwards.
CONSTANT ResyncWindow
Resyncs == Terminated /\ ~valid => badSince + ResyncWindow > (N - 1) * Period

\* A picture built on a missing frame always has a resync on the way
\* (requested, or about to be once the drop is noted).
NoSilentCorruption == ~valid => needSync \/ DropsDue

\* Probe only (expected to fail): a session can end on a bad picture.
EndsValid == Terminated => valid

\* Liveness: the session always ends (no deadlock, no livelock).
Ends == <>Terminated
=============================================================================
