#!/bin/bash
#
# This script checks whether 2 solvers behave the same on a given CNF. The
# criteria is the number of decisions and conflicts performed, as well as the
# exit code.
#

# get to repository base
INPUT="$1"
SOLVER1="$2"
SOLVER2="$3"

usage ()
{
    $0 input 'solver1 + params' 'solver2 + parames'
}


echo "Use input $INPUT"
echo "Run solver1: '$SOLVER1'"
echo "Run solver2: '$SOLVER2'"

# make sure we clean up
trap 'rm -rf $TMPD' EXIT
TMPD=$(mktemp -d)


echo "Run solver 1 ..."
$SOLVER1 "$INPUT" > "$TMPD"/out1.log 2> "$TMPD"/err1.log
STATUS1=$?

echo "Run solver 2 ..."
$SOLVER2 "$INPUT" > "$TMPD"/out2.log 2> "$TMPD"/err2.log
STATUS2=$?

echo "Evaluate ..."
CON1=$(awk '/c conflicts/ {print $4}' "$TMPD"/out1.log)
CON2=$(awk '/c conflicts/ {print $4}' "$TMPD"/out2.log)

DEC1=$(awk '/c decisions/ {print $4}' "$TMPD"/out1.log)
DEC2=$(awk '/c decisions/ {print $4}' "$TMPD"/out2.log)

SUM_CONFLICTS1=$(awk '/c SUM stats conflicts:/ {print $6}' "$TMPD"/out1.log)
SUM_DECISIONS1=$(awk '/c SUM stats decisions:/ {print $6}' "$TMPD"/out1.log)
SUM_RESTARTS1=$(awk '/c SUM stats restarts:/ {print $6}' "$TMPD"/out1.log)
SUM_CONFLICTS2=$(awk '/c SUM stats conflicts:/ {print $6}' "$TMPD"/out2.log)
SUM_DECISIONS2=$(awk '/c SUM stats decisions:/ {print $6}' "$TMPD"/out2.log)
SUM_RESTARTS2=$(awk '/c SUM stats restarts:/ {print $6}' "$TMPD"/out2.log)

STATUS=0

if [ "$CON1" != "$CON2" ]
then
    echo "Conflicts do not match, $CON1 vs $CON2"
    STATUS=1
else
    echo "Conflicts match: $CON1"
fi


if [ "$DEC1" != "$DEC2" ]
then
    echo "Decisions do not match, $DEC1 vs $DEC2"
    STATUS=1
else
    echo "Decisions match: $DEC1"
fi

if [ "$SUM_CONFLICTS1" != "$SUM_CONFLICTS2" ]
then
    echo "Sum of conflicts do not match, $SUM_CONFLICTS1 vs $SUM_CONFLICTS2"
    STATUS=1
else
    echo "Sum of conflicts match: $SUM_CONFLICTS1"
fi

if [ "$SUM_RESTARTS1" != "$SUM_RESTARTS2" ]
then
    echo "Sum of restart do not match, $SUM_RESTARTS1 vs $SUM_RESTARTS2"
    STATUS=1
else
    echo "Sum of restarts match: $SUM_RESTARTS1"
fi

if [ "$SUM_DECISIONS1" != "$SUM_DECISIONS2" ]
then
    echo "Sum of decisions do not match, $SUM_DECISIONS1 vs $SUM_DECISIONS2"
    STATUS=1
else
    echo "Sum of decisions match: $SUM_DECISIONS1"
fi

if [ "$STATUS1" != "$STATUS2" ]
then
    echo "Exit codes do not match, $STATUS1 vs $STATUS2"
    STATUS=1
else
    echo "Exit code match: $STATUS1"
fi

exit $STATUS
