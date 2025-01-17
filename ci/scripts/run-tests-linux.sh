#!/bin/bash
set -e

WORKDIR=/tmp/lantern
GITHUB_OUTPUT=${GITHUB_OUTPUT:-/dev/null}
PG_VERSION=${PG_VERSION:-15}
RUN_TESTS=${RUN_TESTS:-1}

export PGDATA=/etc/postgresql/$PG_VERSION/main

source "$(dirname "$0")/utils.sh"

function stop_current_postgres() {
  # Stop any existing processes
  /usr/lib/postgresql/$PG_VERSION/bin/pg_ctl stop -D $PGDATA
}

function run_pgvector_tests(){
  if [[ $PG_VERSION -gt 12 ]]; then
    pushd /tmp/pgvector
      # Add lantern to load-extension in pgregress
      sed -i '/REGRESS_OPTS \=/ s/$/ --load-extension lantern/'  Makefile
      # Run tests
      make installcheck
    popd
  fi
}

function run_db_tests(){
  if [[ "$RUN_TESTS" == "1" ]]
  then
    cd $WORKDIR/lantern_hnsw/build && \
    make test && \
    make test-parallel && \
    $WORKDIR/lantern_hnsw/cienv/bin/python ../scripts/test_wal.py && \
    run_pgvector_tests && \
    stop_current_postgres
  fi
}

function start_pg() {
  pg_response=$(pg_isready -U postgres 2>&1)

  if [[ $pg_response == *"accepting"* ]]; then
    echo "Postgres already running"
  elif [[ $pg_response == *"rejecting"* ]]; then
    echo "Postgres process is being killed retrying..."
    sleep 1
    start_pg
  else
    # Enable auth without password
    echo "local   all             all                                     trust" >  $PGDATA/pg_hba.conf
    echo "host    all             all             127.0.0.1/32            trust" >>  $PGDATA/pg_hba.conf
    echo "host    all             all             ::1/128                 trust" >>  $PGDATA/pg_hba.conf


    # Set port
    echo "port = 5432" >> ${PGDATA}/postgresql.conf
    # Run postgres database
    POSTGRES_HOST_AUTH_METHOD=trust /usr/lib/postgresql/$PG_VERSION/bin/postgres 1>/tmp/pg-out.log 2>/tmp/pg-error.log &
  fi
}
# Wait for start and run tests
cd /tmp
start_pg && wait_for_pg && run_db_tests
