# Expected Results

Each `out_<test-source-basename>` file stores the expected standard output of its matching test executable when all assertions pass.

The current test programs report a single success-summary line after their assertions complete. They do not print a separate result line for every assertion/test case. Failure messages are written to standard error and the process exits with a non-zero status.

Keep these files synchronized with intentional changes to the test programs' success messages.
