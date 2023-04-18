module.exports = function(grunt) {

	grunt.loadNpmTasks('grunt-contrib-qunit');

	grunt.initConfig({
		pkg: grunt.file.readJSON('package.json'),
		qunit: {
			all: {
				options: {
					urls: ['http://localhost:8910']
				}
			}
		}
	});

	grunt.registerTask('default', ['qunit']);
};
