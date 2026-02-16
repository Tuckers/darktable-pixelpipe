'use strict';

const path = require('path');
const native = require(path.join(__dirname, '..', 'build', 'Release', 'dtpipe.node'));

module.exports = native;
