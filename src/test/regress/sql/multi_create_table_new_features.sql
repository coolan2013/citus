--
-- MULTI_CREATE_TABLE_NEW_FEATURES
--

-- Verify that the GENERATED ... AS IDENTITY feature in PostgreSQL 10
-- is forbidden in distributed tables.

CREATE TABLE table_identity_col (
    id integer GENERATED BY DEFAULT AS IDENTITY PRIMARY KEY,
    payload text );

SELECT create_distributed_table('table_identity_col', 'id', 'append');

SELECT create_distributed_table('table_identity_col', 'id');
SELECT create_distributed_table('table_identity_col', 'text');

SELECT create_reference_table('table_identity_col');
