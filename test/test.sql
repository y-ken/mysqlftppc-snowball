SET GLOBAL snowball_normalization=OFF;
SET GLOBAL snowball_unicode_version="DEFAULT";
SET GLOBAL snowball_algorithm="english";

DROP TABLE IF EXISTS sn;
CREATE TABLE sn (a TEXT, FULLTEXT(a) WITH PARSER snowball) CHARSET latin1;
INSERT INTO sn VALUES ("consignment consistently");
INSERT INTO sn VALUES ("dummy");
INSERT INTO sn VALUES ("dummy");
SELECT COUNT(*) FROM sn WHERE MATCH(a) AGAINST('consigned consisted');
SELECT COUNT(*) FROM sn WHERE MATCH(a) AGAINST('consigned' IN BOOLEAN MODE);
DROP TABLE IF EXISTS sn;

SET GLOBAL snowball_algorithm="fr";
DROP TABLE IF EXISTS sn;
CREATE TABLE sn (a TEXT, FULLTEXT(a) WITH PARSER snowball) CHARSET latin1;
INSERT INTO sn VALUES ("continué continuellement");
INSERT INTO sn VALUES ("dummy");
INSERT INTO sn VALUES ("dummy");
SELECT COUNT(*) FROM sn WHERE MATCH(a) AGAINST('continuait continuelle');
SELECT COUNT(*) FROM sn WHERE MATCH(a) AGAINST('continuait' IN BOOLEAN MODE);
DROP TABLE IF EXISTS sn;

SET GLOBAL snowball_normalization=OFF;
SET GLOBAL snowball_unicode_version="DEFAULT";
SET GLOBAL snowball_algorithm="english"
