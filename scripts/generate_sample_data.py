#!/usr/bin/env python3
################################################################################
# Project: Distributed Financial Backtesting & Risk Engine
# Authors: Talif Pathan, Mohamed Samir Shafat Khan
# Course: CS6650 Scalable Distributed Systems SEC 04 Fall 2025 [BOS-1-TR]
# Date: November 27, 2025
#
# File: generate_sample_data.py
#
# Description:
#   This Python script generates synthetic, yet realistic-looking, historical
#   financial price data (OHLCV) for a set of equity symbols and saves them as
#   CSV files under ./data/sample/.
#
#   The generated data:
#     - Uses a simple geometric random walk with a slight upward drift and
#       configurable volatility to approximate equity price behavior.
#     - Produces daily OHLC (Open, High, Low, Close) and Volume (V) bars.
#     - Skips weekends to better match real-world trading calendars.
#
#   These CSV files are consumed by:
#     - The Raft controllers and workers as input data for backtests.
#     - Integration and scalability tests (e.g., E2 evaluation).
#     - Local single-node cluster tests (e.g., run_local_raft_cluster.sh).
#
# Core Functionality:
#   - Price Series Generation:
#       * generate_price_series(symbol, start_date, end_date, initial_price):
#           - Generates daily OHLCV bars between start_date and end_date.
#           - Models prices using a random walk with drift and volatility.
#           - Excludes weekends (Saturday/Sunday).
#
#   - CSV Persistence:
#       * save_to_csv(symbol, data, output_dir='./data/sample'):
#           - Writes generated OHLCV data to <output_dir>/<symbol>.csv.
#           - Creates the output directory if it does not exist.
#
#   - Batch Data Generation (Main Entrypoint):
#       * When executed as a script:
#           - Generates data for a predefined list of symbols:
#               ['AAPL', 'GOOGL', 'MSFT', 'AMZN', 'TSLA']
#           - Spans a configurable date range (2020-01-01 to 2024-12-31).
#           - Uses symbol-specific initial prices to shape each series.
#
# Role in the Overall System:
#   - Provides a consistent, reproducible source of test data so that:
#       * Backtesting logic can be exercised end-to-end without accessing
#         external market data providers.
#       * Performance and scalability tests can rely on non-trivial datasets.
#       * Developers can run the system on any environment (laptop, cluster)
#         without dealing with licensing or network constraints for real data.
#
# Assumptions & Design Choices:
#   - Trading Calendar:
#       * Only filters out weekends; no special handling for market holidays.
#   - Price Model:
#       * Simple random walk with a small positive drift and Gaussian noise.
#       * Not meant for financial modeling accuracy; it is "realistic enough"
#         for systems testing and demos.
#   - Volumes:
#       * Modeled with a Gaussian distribution around 1,000,000 shares/day,
#         with moderate variance.
#
# Dependencies:
#   - Python 3.x
#   - Standard library only:
#       * csv, random, datetime, os
#
# Related Scripts:
#   - run_local_raft_cluster.sh        : Uses these CSVs to exercise local Raft
#                                        cluster and worker pool.
#   - e2_scalability_evaluation.sh     : Uses these CSVs for scalability tests.
#   - deploy_to_khoury.sh              : Assumes data is present or generated
#                                        prior to deployment.
#
################################################################################

#===============================================================================
# TABLE OF CONTENTS
#===============================================================================
#
# 1. MODULE OVERVIEW
# 2. IMPORTS
# 3. generate_price_series()
#    3.1 Function Purpose
#    3.2 Price Model (Drift + Volatility)
#    3.3 Weekend Skipping Logic
#    3.4 OHLCV Construction
# 4. save_to_csv()
#    4.1 Directory Creation
#    4.2 CSV Schema
# 5. MAIN SCRIPT EXECUTION
#    5.1 Default Symbol Universe
#    5.2 Date Range and Initial Prices
#    5.3 Batch Generation Loop
# 6. USAGE EXAMPLES
# 7. COMMON PITFALLS & NOTES
# 8. FAQ
#
#===============================================================================

"""
Sample data generator for the Distributed Financial Backtesting & Risk Engine.

This module provides:
    - A helper to generate synthetic OHLCV time series using a simple random
      walk model with drift and volatility.
    - A helper to persist that data to CSV in a standard format.
    - A main entrypoint that generates data for several well-known equity
      symbols across a multi-year period.

Typical usage:

    $ python3 scripts/generate_sample_data.py

After running, you should see CSV files like:
    ./data/sample/AAPL.csv
    ./data/sample/GOOGL.csv
    ...

These files can then be consumed by the Raft controller / worker processes
and integration tests.
"""

#===============================================================================
# 2. IMPORTS
#-------------------------------------------------------------------------------
# Standard library imports; no external dependencies to keep this script easy
# to run on any environment (Khoury cluster or local laptop).
import csv
import random
from datetime import datetime, timedelta
import os


#===============================================================================
# 3. generate_price_series()
#===============================================================================
def generate_price_series(symbol, start_date, end_date, initial_price=100.0):
    """
    Generate a synthetic daily OHLCV price series for a given symbol.

    Parameters
    ----------
    symbol : str
        Ticker symbol for the asset (e.g., 'AAPL', 'MSFT').
        Note: `symbol` is currently only used for logging in the caller;
        the time series itself does not embed the symbol.
    start_date : datetime
        Inclusive start date of the historical series.
    end_date : datetime
        Inclusive end date of the historical series.
    initial_price : float, optional
        Starting price at start_date. This anchors the series and helps
        differentiate symbols (e.g., TSLA vs MSFT).

    Returns
    -------
    list of dict
        Each dict represents one trading day with keys:
            'Date'   : YYYY-MM-DD (string)
            'Open'   : opening price (string, 2 decimal places)
            'High'   : session high price (string, 2 decimal places)
            'Low'    : session low price (string, 2 decimal places)
            'Close'  : closing price (string, 2 decimal places)
            'Volume' : integer traded volume (approx around 1,000,000)

    Notes
    -----
    - Skips Saturdays and Sundays using datetime.weekday() >= 5.
    - Uses a simple geometric random walk:
          daily_return = trend + N(0, volatility)
          price_{t+1} = price_t * (1 + daily_return)
    - OHLC prices are derived from the underlying "current_price" using
      small random perturbations to create realistic intraday ranges.
    """
    # Current simulation date and price.
    current_date = start_date
    current_price = initial_price

    # Accumulator for all daily OHLCV records.
    data = []

    while current_date <= end_date:
        #-----------------------------------------------------------------------
        # 3.3 Weekend Skipping Logic
        #-----------------------------------------------------------------------
        # weekday() returns:
        #   0 = Monday, 1 = Tuesday, ..., 5 = Saturday, 6 = Sunday
        # We skip Saturdays (5) and Sundays (6) to approximate trading days.
        if current_date.weekday() >= 5:
            current_date += timedelta(days=1)
            continue

        #-----------------------------------------------------------------------
        # 3.2 Price Model (Drift + Volatility)
        #-----------------------------------------------------------------------
        # trend:
        #   - Small positive drift to simulate a long-term upward bias.
        # volatility:
        #   - Standard deviation of daily returns, representing daily price
        #     fluctuations around the trend.
        trend = 0.001   # ≈ 0.1% expected daily drift
        volatility = 0.02  # ≈ 2% daily volatility

        # daily_return:
        #   - Base drift + random Gaussian shock.
        daily_return = trend + random.gauss(0, volatility)
        current_price *= (1 + daily_return)

        #-----------------------------------------------------------------------
        # 3.4 OHLCV Construction
        #-----------------------------------------------------------------------
        # We build Open, High, Low, Close and Volume from current_price:
        #
        #   - Close: the simulated current price after applying the random walk.
        #   - Open:  small random perturbation around current_price.
        #   - High:  slightly above max(Open, Close).
        #   - Low:   slightly below min(Open, Close).
        #   - Volume: Gaussian around 1,000,000 with std dev 200,000.
        #
        # We format prices to 2 decimal places, matching typical equity data.
        open_price = current_price * (1 + random.gauss(0, 0.005))
        close_price = current_price
        high_price = max(open_price, close_price) * (1 + abs(random.gauss(0, 0.01)))
        low_price = min(open_price, close_price) * (1 - abs(random.gauss(0, 0.01)))
        volume = int(random.gauss(1000000, 200000))

        # Append the day's record as a dict with a fixed schema matching
        # typical OHLCV CSV files used in backtesting engines.
        data.append({
            'Date': current_date.strftime('%Y-%m-%d'),
            'Open': f'{open_price:.2f}',
            'High': f'{high_price:.2f}',
            'Low': f'{low_price:.2f}',
            'Close': f'{close_price:.2f}',
            'Volume': volume
        })

        # Move to the next calendar day (trading day will be next non-weekend).
        current_date += timedelta(days=1)

    return data


#===============================================================================
# 4. save_to_csv()
#===============================================================================
def save_to_csv(symbol, data, output_dir='./data/sample'):
    """
    Persist a list of OHLCV bars to a CSV file.

    Parameters
    ----------
    symbol : str
        Ticker symbol (used to derive the filename: <symbol>.csv).
    data : list of dict
        List of daily bars as returned by generate_price_series().
        Each dict is expected to have keys: Date, Open, High, Low, Close, Volume.
    output_dir : str, optional
        Directory into which the CSV file will be written. The directory is
        created if it does not already exist.

    Side Effects
    ------------
    - Creates output_dir if missing.
    - Writes (or overwrites) '<output_dir>/<symbol>.csv'.

    CSV Schema
    ----------
    Columns (in order):
        Date, Open, High, Low, Close, Volume

    Example
    -------
    >>> data = generate_price_series('DEMO', start, end, 100.0)
    >>> save_to_csv('DEMO', data, output_dir='./data/sample')
    """
    # Ensure the output directory exists; 'exist_ok=True' makes this idempotent.
    os.makedirs(output_dir, exist_ok=True)

    # Build full file path; file will be named like 'AAPL.csv', 'MSFT.csv', etc.
    filepath = os.path.join(output_dir, f'{symbol}.csv')

    # Open the file for writing. newline='' is recommended for csv module.
    with open(filepath, 'w', newline='') as f:
        writer = csv.DictWriter(
            f,
            fieldnames=['Date', 'Open', 'High', 'Low', 'Close', 'Volume']
        )
        writer.writeheader()
        writer.writerows(data)

    # Console feedback for the user running the script.
    print(f'Generated {len(data)} bars for {symbol} -> {filepath}')


#===============================================================================
# 5. MAIN SCRIPT EXECUTION
#===============================================================================
if __name__ == '__main__':
    #---------------------------------------------------------------------------
    # 5.1 Default Symbol Universe
    #---------------------------------------------------------------------------
    # These are well-known large-cap equities chosen for familiarity. The names
    # themselves do not affect the generated series, but they help make tests
    # easier to reason about.
    symbols = ['AAPL', 'GOOGL', 'MSFT', 'AMZN', 'TSLA']

    #---------------------------------------------------------------------------
    # 5.2 Date Range for Generated Data
    #---------------------------------------------------------------------------
    # The date range is inclusive of both start_date and end_date and will
    # generate data for all weekdays in this interval.
    #
    # Adjust these if you want a shorter dataset (faster tests) or a longer one
    # (more realistic backtest horizons).
    start_date = datetime(2020, 1, 1)
    end_date = datetime(2024, 12, 31)

    #---------------------------------------------------------------------------
    # 5.3 Initial Price per Symbol
    #---------------------------------------------------------------------------
    # Initial prices roughly approximate real-world price levels around early
    # 2020 for these symbols, but exact values are not critical.
    #
    # They mainly ensure that:
    #   - TSLA starts higher than MSFT.
    #   - GOOGL/AMZN appear as higher-priced tickers.
    initial_prices = {
        'AAPL': 300.0,
        'GOOGL': 1400.0,
        'MSFT': 160.0,
        'AMZN': 1850.0,
        'TSLA': 430.0
    }

    #---------------------------------------------------------------------------
    # 5.4 Batch Generation Loop
    #---------------------------------------------------------------------------
    # For each symbol:
    #   - Generate a full price series using its designated initial price.
    #   - Persist it to CSV under ./data/sample/<SYMBOL>.csv
    for symbol in symbols:
        data = generate_price_series(symbol, start_date, end_date, initial_prices[symbol])
        save_to_csv(symbol, data)

    # Final user-facing message summarizing where to find generated files.
    print('\nSample data generation complete!')
    print('Files created in ./data/sample/')


#===============================================================================
# 6. USAGE EXAMPLES
#===============================================================================
#
# From the project root:
#
#   1) Generate default sample data:
#        $ python3 scripts/generate_sample_data.py
#
#   2) Generate a shorter series (interactive use):
#
#        from datetime import datetime
#        from scripts.generate_sample_data import generate_price_series, save_to_csv
#
#        start = datetime(2024, 1, 1)
#        end = datetime(2024, 3, 31)
#        data = generate_price_series('DEMO', start, end, initial_price=50.0)
#        save_to_csv('DEMO', data, output_dir='./data/sample')
#
#   3) Integrate into tests:
#        - Call generate_price_series() in a setup step to create in-memory
#          data for unit tests without hitting disk.
#
#===============================================================================
# 7. COMMON PITFALLS & NOTES
#===============================================================================
#
# - Randomness and Reproducibility:
#     By default, the script does NOT fix the random seed. This means:
#       * Running it multiple times will produce slightly different price paths.
#     If you need reproducible data for a report or regression tests, set a
#     seed at the top of the script or in your caller:
#
#         import random
#         random.seed(42)
#
# - Trading Calendar:
#     The script only skips weekends; it does not remove market holidays.
#     For more realism, you could:
#       * Maintain a holiday calendar and skip those dates as well.
#
# - Data Volume:
#     Generating 5 years of daily data for multiple symbols is usually fast,
#     but if you add many more symbols or extend the date range significantly,
#     file sizes will grow and generation time will increase.
#
# - CSV Overwrite:
#     If a file already exists (e.g., ./data/sample/AAPL.csv), it will be
#     overwritten without prompting. Back up any curated data before re-running.
#
#===============================================================================
# 8. FAQ
#===============================================================================
#
# Q1: Can I add more symbols?
# A1: Yes. Modify the `symbols` list and `initial_prices` dictionary in the
#     __main__ block. Make sure each new symbol has an entry in initial_prices.
#
# Q2: How do I change the volatility or trend?
# A2: Edit `trend` and `volatility` in generate_price_series():
#       trend = 0.001      # daily drift
#       volatility = 0.02  # daily volatility
#     You can also expose them as function parameters if you want to vary them
#     per symbol or test.
#
# Q3: How do I ensure deterministic output for grading or regression tests?
# A3: Set the random seed before generating data:
#       import random
#       random.seed(12345)
#     or add `random.seed(12345)` at the top of this script (before calling
#     generate_price_series in the __main__ block).
#
# Q4: Why store prices as strings in the dict (formatted with 2 decimals)?
# A4: This mirrors the typical CSV representation and ensures consistent
#     formatting in the output. If you prefer floats internally, you can store
#     them as float and format only when writing the CSV.
#
# Q5: Can I shorten the date range for faster tests?
# A5: Absolutely. In the __main__ block, change:
#       start_date = datetime(2020, 1, 1)
#       end_date = datetime(2024, 12, 31)
#     to a smaller range, e.g., a single year or even a few months.
#
# Q6: How big are the generated files?
# A6: Roughly:
#     - A few years of daily (weekday-only) data per symbol → a few thousand
#       rows per CSV.
#     - For 5 symbols and ~5 years, this is still quite manageable and ideal
#       for most testing workloads.
#
# End of generate_sample_data.py
#===============================================================================
